// Copyright (c) 2025-2026 fei_cong(https://github.com/feicong/feicong-course)

#include <arpa/inet.h>
#include <errno.h>
#include <linux/genetlink.h>
#include <linux/netlink.h>
#include <linux/nl80211.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "../include/cdh_wifi_scan_model.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace {

constexpr size_t kBufferSize = 4096;

struct Options {
  std::string interface_name = "wlan0";
  std::string ssid = "VirtWifi";
  std::string ipv4_cidr;
  std::string gateway;
  int settle_ms = 2500;
};

size_t align4(size_t value) { return (value + 3U) & ~size_t{3U}; }

bool netlinkMessageOk(const nlmsghdr* header, int remaining) {
  return remaining >= static_cast<int>(sizeof(*header)) &&
         header->nlmsg_len >= sizeof(*header) &&
         header->nlmsg_len <= static_cast<uint32_t>(remaining);
}

bool addAttribute(nlmsghdr* header, size_t capacity, uint16_t type,
                  const void* data, size_t length) {
  const size_t attribute_length = sizeof(nlattr) + length;
  const size_t offset = align4(header->nlmsg_len);
  if (offset + align4(attribute_length) > capacity) return false;
  auto* attribute = reinterpret_cast<nlattr*>(
      reinterpret_cast<char*>(header) + offset);
  attribute->nla_type = type;
  attribute->nla_len = static_cast<uint16_t>(attribute_length);
  std::memcpy(reinterpret_cast<char*>(attribute) + sizeof(*attribute), data,
              length);
  std::memset(reinterpret_cast<char*>(attribute) + attribute_length, 0,
              align4(attribute_length) - attribute_length);
  header->nlmsg_len = static_cast<uint32_t>(offset + align4(attribute_length));
  return true;
}

int openNetlinkSocket(int protocol) {
  const int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, protocol);
  if (fd < 0) return -1;
  sockaddr_nl local = {};
  local.nl_family = AF_NETLINK;
  if (bind(fd, reinterpret_cast<sockaddr*>(&local), sizeof(local)) < 0) {
    close(fd);
    return -1;
  }
  timeval timeout = {.tv_sec = 5, .tv_usec = 0};
  if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0) {
    close(fd);
    return -1;
  }
  return fd;
}

bool sendNetlinkMessage(int fd, nlmsghdr* header) {
  sockaddr_nl peer = {};
  peer.nl_family = AF_NETLINK;
  return sendto(fd, header, header->nlmsg_len, 0,
                reinterpret_cast<sockaddr*>(&peer), sizeof(peer)) >= 0;
}

bool waitForAck(int fd, uint32_t sequence) {
  char buffer[kBufferSize];
  for (;;) {
    const ssize_t received = recv(fd, buffer, sizeof(buffer), 0);
    if (received < 0) return false;
    int remaining = static_cast<int>(received);
    for (auto* header = reinterpret_cast<nlmsghdr*>(buffer);
         netlinkMessageOk(header, remaining);
         header = NLMSG_NEXT(header, remaining)) {
      if (header->nlmsg_seq != sequence || header->nlmsg_type != NLMSG_ERROR) {
        continue;
      }
      const auto* error = reinterpret_cast<const nlmsgerr*>(NLMSG_DATA(header));
      if (error->error != 0) {
        errno = -error->error;
        return false;
      }
      return true;
    }
  }
}

int resolveNl80211Family(int fd, uint32_t sequence) {
  char request_buffer[kBufferSize] = {};
  auto* header = reinterpret_cast<nlmsghdr*>(request_buffer);
  header->nlmsg_len = NLMSG_LENGTH(GENL_HDRLEN);
  header->nlmsg_type = GENL_ID_CTRL;
  header->nlmsg_flags = NLM_F_REQUEST;
  header->nlmsg_seq = sequence;
  auto* generic = reinterpret_cast<genlmsghdr*>(NLMSG_DATA(header));
  generic->cmd = CTRL_CMD_GETFAMILY;
  generic->version = 1;
  constexpr char kFamilyName[] = "nl80211";
  if (!addAttribute(header, sizeof(request_buffer), CTRL_ATTR_FAMILY_NAME,
                    kFamilyName, sizeof(kFamilyName)) ||
      !sendNetlinkMessage(fd, header)) {
    return -1;
  }

  char response_buffer[kBufferSize];
  for (;;) {
    const ssize_t received = recv(fd, response_buffer, sizeof(response_buffer), 0);
    if (received < 0) return -1;
    int remaining = static_cast<int>(received);
    for (header = reinterpret_cast<nlmsghdr*>(response_buffer);
         netlinkMessageOk(header, remaining);
         header = NLMSG_NEXT(header, remaining)) {
      if (header->nlmsg_seq != sequence) continue;
      if (header->nlmsg_type == NLMSG_ERROR) {
        const auto* error = reinterpret_cast<const nlmsgerr*>(NLMSG_DATA(header));
        if (error->error != 0) errno = -error->error;
        return -1;
      }
      generic = reinterpret_cast<genlmsghdr*>(NLMSG_DATA(header));
      int attribute_bytes = static_cast<int>(header->nlmsg_len) - NLMSG_HDRLEN -
                            GENL_HDRLEN;
      auto* attribute = reinterpret_cast<nlattr*>(
          reinterpret_cast<char*>(generic) + GENL_HDRLEN);
      while (attribute_bytes >= static_cast<int>(sizeof(*attribute)) &&
             attribute->nla_len >= sizeof(*attribute) &&
             attribute->nla_len <= attribute_bytes) {
        if (attribute->nla_type == CTRL_ATTR_FAMILY_ID &&
            attribute->nla_len >= sizeof(*attribute) + sizeof(uint16_t)) {
          uint16_t family_id = 0;
          std::memcpy(&family_id,
                      reinterpret_cast<char*>(attribute) + sizeof(*attribute),
                      sizeof(family_id));
          return family_id;
        }
        const int step = static_cast<int>(align4(attribute->nla_len));
        attribute_bytes -= step;
        attribute = reinterpret_cast<nlattr*>(
            reinterpret_cast<char*>(attribute) + step);
      }
    }
  }
}

bool setInterfaceUp(const std::string& interface_name) {
  const int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
  if (fd < 0) return false;
  ifreq request = {};
  std::strncpy(request.ifr_name, interface_name.c_str(), IFNAMSIZ - 1);
  bool ok = ioctl(fd, SIOCGIFFLAGS, &request) == 0;
  if (ok) {
    request.ifr_flags |= IFF_UP;
    ok = ioctl(fd, SIOCSIFFLAGS, &request) == 0;
  }
  const int saved_errno = errno;
  close(fd);
  errno = saved_errno;
  return ok;
}

bool connectOpenNetwork(const Options& options) {
  const unsigned int interface_index = if_nametoindex(options.interface_name.c_str());
  if (interface_index == 0) return false;
  const int fd = openNetlinkSocket(NETLINK_GENERIC);
  if (fd < 0) return false;
  const int family_id = resolveNl80211Family(fd, 1);
  if (family_id < 0) {
    close(fd);
    return false;
  }

  char buffer[kBufferSize] = {};
  auto* header = reinterpret_cast<nlmsghdr*>(buffer);
  header->nlmsg_len = NLMSG_LENGTH(GENL_HDRLEN);
  header->nlmsg_type = static_cast<uint16_t>(family_id);
  header->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
  header->nlmsg_seq = 2;
  auto* generic = reinterpret_cast<genlmsghdr*>(NLMSG_DATA(header));
  generic->cmd = NL80211_CMD_CONNECT;
  generic->version = 1;
  const uint32_t index = interface_index;
  const uint32_t authentication = NL80211_AUTHTYPE_OPEN_SYSTEM;
  const bool prepared =
      addAttribute(header, sizeof(buffer), NL80211_ATTR_IFINDEX, &index,
                   sizeof(index)) &&
      addAttribute(header, sizeof(buffer), NL80211_ATTR_SSID,
                   options.ssid.data(), options.ssid.size()) &&
      addAttribute(header, sizeof(buffer), NL80211_ATTR_AUTH_TYPE,
                   &authentication, sizeof(authentication));
  const bool ok = prepared && sendNetlinkMessage(fd, header) && waitForAck(fd, 2);
  const int saved_errno = errno;
  close(fd);
  errno = saved_errno;
  return ok;
}

bool parseCidr(const std::string& cidr, in_addr* address, in_addr* netmask) {
  const size_t slash = cidr.find('/');
  if (slash == std::string::npos) return false;
  char* end = nullptr;
  const long prefix = std::strtol(cidr.c_str() + slash + 1, &end, 10);
  if (!end || *end != '\0' || prefix < 1 || prefix > 32) return false;
  const std::string address_text = cidr.substr(0, slash);
  if (inet_pton(AF_INET, address_text.c_str(), address) != 1) return false;
  const uint32_t mask = prefix == 32 ? 0xffffffffU : 0xffffffffU << (32 - prefix);
  netmask->s_addr = htonl(mask);
  return true;
}

bool setInterfaceAddress(const std::string& interface_name,
                         const std::string& cidr) {
  in_addr address = {};
  in_addr netmask = {};
  if (!parseCidr(cidr, &address, &netmask)) {
    errno = EINVAL;
    return false;
  }
  const int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
  if (fd < 0) return false;
  ifreq request = {};
  std::strncpy(request.ifr_name, interface_name.c_str(), IFNAMSIZ - 1);
  auto* socket_address = reinterpret_cast<sockaddr_in*>(&request.ifr_addr);
  socket_address->sin_family = AF_INET;
  socket_address->sin_addr = address;
  bool ok = ioctl(fd, SIOCSIFADDR, &request) == 0;
  if (ok) {
    socket_address = reinterpret_cast<sockaddr_in*>(&request.ifr_netmask);
    socket_address->sin_family = AF_INET;
    socket_address->sin_addr = netmask;
    ok = ioctl(fd, SIOCSIFNETMASK, &request) == 0;
  }
  const int saved_errno = errno;
  close(fd);
  errno = saved_errno;
  return ok;
}

bool replaceDefaultRoute(const std::string& interface_name,
                         const std::string& gateway_text) {
  in_addr gateway = {};
  if (inet_pton(AF_INET, gateway_text.c_str(), &gateway) != 1) {
    errno = EINVAL;
    return false;
  }
  const unsigned int interface_index = if_nametoindex(interface_name.c_str());
  if (interface_index == 0) return false;
  const int fd = openNetlinkSocket(NETLINK_ROUTE);
  if (fd < 0) return false;

  struct RouteRequest {
    nlmsghdr header;
    rtmsg route;
    char attributes[128];
  } request = {};
  request.header.nlmsg_len = NLMSG_LENGTH(sizeof(rtmsg));
  request.header.nlmsg_type = RTM_NEWROUTE;
  request.header.nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK | NLM_F_CREATE |
                               NLM_F_REPLACE;
  request.header.nlmsg_seq = 3;
  request.route.rtm_family = AF_INET;
  request.route.rtm_table = RT_TABLE_MAIN;
  request.route.rtm_protocol = RTPROT_BOOT;
  request.route.rtm_scope = RT_SCOPE_UNIVERSE;
  request.route.rtm_type = RTN_UNICAST;
  const uint32_t index = interface_index;
  const bool prepared =
      addAttribute(&request.header, sizeof(request), RTA_GATEWAY, &gateway,
                   sizeof(gateway)) &&
      addAttribute(&request.header, sizeof(request), RTA_OIF, &index,
                   sizeof(index));
  const bool ok = prepared && sendNetlinkMessage(fd, &request.header) &&
                  waitForAck(fd, 3);
  const int saved_errno = errno;
  close(fd);
  errno = saved_errno;
  return ok;
}

bool parseInt(const char* text, int minimum, int maximum, int* out) {
  char* end = nullptr;
  const long value = std::strtol(text, &end, 10);
  if (!end || *end != '\0' || value < minimum || value > maximum) return false;
  *out = static_cast<int>(value);
  return true;
}

bool parseOptions(int argc, char** argv, Options* options) {
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    if (index + 1 >= argc) return false;
    const char* value = argv[++index];
    if (argument == "--interface") {
      options->interface_name = value;
    } else if (argument == "--ssid") {
      options->ssid = value;
    } else if (argument == "--ipv4") {
      options->ipv4_cidr = value;
    } else if (argument == "--gateway") {
      options->gateway = value;
    } else if (argument == "--settle-ms") {
      if (!parseInt(value, 0, 30000, &options->settle_ms)) return false;
    } else {
      return false;
    }
  }
  return !options->interface_name.empty() &&
         options->interface_name.size() < IFNAMSIZ && !options->ssid.empty() &&
         options->ssid.size() <= 32 &&
         (options->ipv4_cidr.empty() == options->gateway.empty());
}

bool sleepMilliseconds(int milliseconds) {
  timespec remaining = {
      .tv_sec = milliseconds / 1000,
      .tv_nsec = static_cast<long>(milliseconds % 1000) * 1000000L,
  };
  while (nanosleep(&remaining, &remaining) != 0) {
    if (errno != EINTR) return false;
  }
  return true;
}

int profileCommand(int argc, char** argv) {
  cdh_wifi::ScanModel model;
  std::string error;
  if (argc == 3 && std::string(argv[2]) == "get") {
    if (!cdh_wifi::ReadScanModel(cdh_wifi::kScanModelPath, &model, &error)) {
      std::fprintf(stderr, "read scan profile: %s\n", error.c_str());
      return 20;
    }
  } else if (argc == 4 && std::string(argv[2]) == "set") {
    if (!cdh_wifi::ParseScanModel(argv[3], &model, &error)) {
      std::fprintf(stderr, "parse scan profile: %s\n", error.c_str());
      return 2;
    }
    if (!cdh_wifi::WriteScanModel(cdh_wifi::kScanModelPath, model, &error)) {
      std::fprintf(stderr, "write scan profile: %s\n", error.c_str());
      return 21;
    }
    if (!cdh_wifi::ReadScanModel(cdh_wifi::kScanModelPath, &model, &error)) {
      std::fprintf(stderr, "read scan profile after write: %s\n",
                   error.c_str());
      return 22;
    }
  } else {
    std::fprintf(stderr,
                 "usage: %s profile get | profile set ENCODED_PROFILE\n",
                 argv[0]);
    return 2;
  }
  std::printf("scan_profile=%s\nstatus=OK\n",
              cdh_wifi::EncodeScanModel(model).c_str());
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc >= 2 && std::string(argv[1]) == "profile") {
    return profileCommand(argc, argv);
  }
  Options options;
  if (!parseOptions(argc, argv, &options)) {
    std::fprintf(stderr,
                 "usage: %s [--interface IFACE] [--ssid SSID] "
                 "[--settle-ms MS] [--ipv4 ADDRESS/PREFIX --gateway ADDRESS]\n",
                 argv[0]);
    return 2;
  }
  if (!setInterfaceUp(options.interface_name)) {
    std::perror("set interface up");
    return 10;
  }
  if (!connectOpenNetwork(options)) {
    std::perror("connect open Wi-Fi network");
    return 11;
  }
  if (!sleepMilliseconds(options.settle_ms)) {
    std::perror("wait for Wi-Fi association");
    return 14;
  }
  if (!options.ipv4_cidr.empty() &&
      !setInterfaceAddress(options.interface_name, options.ipv4_cidr)) {
    std::perror("set interface IPv4 address");
    return 12;
  }
  if (!options.gateway.empty() &&
      !replaceDefaultRoute(options.interface_name, options.gateway)) {
    std::perror("replace default route");
    return 13;
  }
  std::printf("interface=%s\nssid=%s\nipv4=%s\ngateway=%s\nstatus=OK\n",
              options.interface_name.c_str(), options.ssid.c_str(),
              options.ipv4_cidr.empty() ? "unchanged" : options.ipv4_cidr.c_str(),
              options.gateway.empty() ? "unchanged" : options.gateway.c_str());
  return 0;
}
