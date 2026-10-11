// Copyright (c) 2025-2026 fei_cong(https://github.com/feicong/feicong-course)
#include <fcntl.h>
#include <inttypes.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaError.h>
#include <media/NdkMediaFormat.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <dlfcn.h>

static const char kEncoderName[] = "c2.v4l2.avc.encoder";
static const char kExpectedMIME[] = "video/avc";
static const int32_t kDefaultWidth = 1280;
static const int32_t kDefaultHeight = 720;
static const int32_t kDefaultFrameRate = 30;
static const int32_t kDefaultFrameCount = 30;
static const int32_t kDefaultBitrate = 4000000;
static const int64_t kDequeueTimeoutUs = 20000;
static const int64_t kDefaultDeadlineSec = 30;
static const int32_t kColorFormatFlexibleYUV = 0x7F420888;

typedef struct {
    const char *codec;
    int32_t width;
    int32_t height;
    int32_t frameRate;
    int32_t frameCount;
    int32_t bitrate;
    const char *outputPath;
    int64_t deadlineSec;
} EncodeOptions;

static int64_t monotonicNanoseconds(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return 0;
    }
    return (int64_t)now.tv_sec * 1000000000LL + now.tv_nsec;
}

static int failStatus(const char *stage, media_status_t status) {
    fprintf(stderr, "result=fail stage=%s status=%d\n", stage, status);
    return 1;
}

static int failMessage(const char *stage, const char *message) {
    fprintf(stderr, "result=fail stage=%s message=%s\n", stage, message);
    return 1;
}

static void defaultOptions(EncodeOptions *options) {
    options->codec = kEncoderName;
    options->width = kDefaultWidth;
    options->height = kDefaultHeight;
    options->frameRate = kDefaultFrameRate;
    options->frameCount = kDefaultFrameCount;
    options->bitrate = kDefaultBitrate;
    options->outputPath = "/data/local/tmp/encode-out.h264";
    options->deadlineSec = kDefaultDeadlineSec;
}

static int parseOptions(int argc, char **argv, EncodeOptions *options) {
    defaultOptions(options);
    for (int index = 1; index < argc; ++index) {
        const char *argument = argv[index];
        if (strcmp(argument, "--codec") == 0) {
            if (++index >= argc) {
                return failMessage("arguments", "missing-codec-value");
            }
            options->codec = argv[index];
            continue;
        }
        if (strcmp(argument, "--width") == 0) {
            if (++index >= argc) {
                return failMessage("arguments", "missing-width-value");
            }
            options->width = (int32_t)strtol(argv[index], NULL, 10);
            continue;
        }
        if (strcmp(argument, "--height") == 0) {
            if (++index >= argc) {
                return failMessage("arguments", "missing-height-value");
            }
            options->height = (int32_t)strtol(argv[index], NULL, 10);
            continue;
        }
        if (strcmp(argument, "--frames") == 0) {
            if (++index >= argc) {
                return failMessage("arguments", "missing-frames-value");
            }
            options->frameCount = (int32_t)strtol(argv[index], NULL, 10);
            continue;
        }
        if (strcmp(argument, "--fps") == 0) {
            if (++index >= argc) {
                return failMessage("arguments", "missing-fps-value");
            }
            options->frameRate = (int32_t)strtol(argv[index], NULL, 10);
            continue;
        }
        if (strcmp(argument, "--bitrate") == 0) {
            if (++index >= argc) {
                return failMessage("arguments", "missing-bitrate-value");
            }
            options->bitrate = (int32_t)strtol(argv[index], NULL, 10);
            continue;
        }
        if (strcmp(argument, "--output") == 0) {
            if (++index >= argc) {
                return failMessage("arguments", "missing-output-value");
            }
            options->outputPath = argv[index];
            continue;
        }
        if (strcmp(argument, "--deadline") == 0) {
            if (++index >= argc) {
                return failMessage("arguments", "missing-deadline-value");
            }
            options->deadlineSec = strtoll(argv[index], NULL, 10);
            continue;
        }
        return failMessage("arguments",
                           "usage:mediandk_encode_client "
                           "[--codec NAME] [--width W] [--height H] "
                           "[--frames N] [--fps N] [--bitrate BPS] "
                           "[--output PATH] [--deadline SEC]");
    }
    if (options->width <= 0 || options->height <= 0 || options->frameCount <= 0 ||
        options->frameRate <= 0 || options->bitrate <= 0 || options->deadlineSec <= 0) {
        return failMessage("arguments", "invalid-dimensions-or-counts");
    }
    return 0;
}

static void fillNV12Frame(uint8_t *buffer, size_t capacity, int32_t width, int32_t height,
                          int32_t frameIndex) {
    const size_t ySize = (size_t)width * (size_t)height;
    const size_t uvSize = ySize / 2U;
    const size_t required = ySize + uvSize;
    if (capacity < required) {
        return;
    }
    const uint8_t yBase = (uint8_t)(16 + (frameIndex * 7) % 200);
    const uint8_t uBase = (uint8_t)(96 + (frameIndex * 5) % 64);
    const uint8_t vBase = (uint8_t)(128 + (frameIndex * 3) % 64);
    for (size_t row = 0; row < (size_t)height; ++row) {
        memset(buffer + row * (size_t)width, (int)(yBase + (row % 16)), (size_t)width);
    }
    uint8_t *uv = buffer + ySize;
    for (size_t row = 0; row < (size_t)height / 2U; ++row) {
        for (size_t column = 0; column < (size_t)width; column += 2U) {
            uv[row * (size_t)width + column] = uBase;
            uv[row * (size_t)width + column + 1U] = vBase;
        }
    }
}

static bool isTryAgainLater(ssize_t outputIndex) {
    return outputIndex == AMEDIACODEC_INFO_TRY_AGAIN_LATER || outputIndex == -10000;
}

static bool isOutputFormatChanged(ssize_t outputIndex) {
    return outputIndex == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED || outputIndex == -10001;
}

static bool isOutputBuffersChanged(ssize_t outputIndex) {
    return outputIndex == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED || outputIndex == -10002;
}

static bool looksLikeAnnexBStart(const uint8_t *data, size_t size) {
    if (size >= 4 && data[0] == 0 && data[1] == 0 && data[2] == 0 && data[3] == 1) {
        return true;
    }
    if (size >= 3 && data[0] == 0 && data[1] == 0 && data[2] == 1) {
        return true;
    }
    return false;
}

static int startBinderThreadPool(void) {
    void *binderNdk = dlopen("libbinder_ndk.so", RTLD_NOW);
    if (binderNdk == NULL) {
        return failMessage("binder", "dlopen-libbinder_ndk-failed");
    }
    void (*setMax)(int32_t) =
            (void (*)(int32_t))dlsym(binderNdk, "ABinderProcess_setThreadPoolMaxThreadCount");
    void (*startPool)(void) = (void (*)(void))dlsym(binderNdk, "ABinderProcess_startThreadPool");
    if (setMax == NULL || startPool == NULL) {
        return failMessage("binder", "missing-ABinderProcess-threadpool-symbols");
    }
    setMax(4);
    startPool();
    return 0;
}

int main(int argc, char **argv) {
    EncodeOptions options;
    if (parseOptions(argc, argv, &options) != 0) {
        return 1;
    }

    if (startBinderThreadPool() != 0) {
        return 1;
    }

    int result = 1;
    int outputFile = -1;
    AMediaCodec *codec = NULL;
    AMediaFormat *format = NULL;
    bool inputEOS = false;
    bool outputEOS = false;
    int queuedFrames = 0;
    int encodedFrames = 0;
    int codecConfigFrames = 0;
    int outputBuffersSeen = 0;
    int formatChanges = 0;
    size_t outputBytes = 0;
    int64_t deadline = 0;

    outputFile = open(options.outputPath, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (outputFile < 0) {
        result = failMessage("open", "cannot-open-output");
        goto cleanup;
    }

    format = AMediaFormat_new();
    if (format == NULL) {
        result = failMessage("format", "create-failed");
        goto cleanup;
    }
    AMediaFormat_setString(format, AMEDIAFORMAT_KEY_MIME, kExpectedMIME);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_WIDTH, options.width);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_HEIGHT, options.height);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_COLOR_FORMAT, kColorFormatFlexibleYUV);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_FRAME_RATE, options.frameRate);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_BIT_RATE, options.bitrate);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_I_FRAME_INTERVAL, 1);

    codec = AMediaCodec_createCodecByName(options.codec);
    if (codec == NULL) {
        result = failMessage("codec", "create-v4l2-avc-encoder-failed");
        goto cleanup;
    }
    media_status_t status = AMediaCodec_configure(codec, format, NULL, NULL,
                                                  AMEDIACODEC_CONFIGURE_FLAG_ENCODE);
    if (status != AMEDIA_OK) {
        result = failStatus("configure", status);
        goto cleanup;
    }
    status = AMediaCodec_start(codec);
    if (status != AMEDIA_OK) {
        result = failStatus("start", status);
        goto cleanup;
    }

    fprintf(stderr,
            "encode codec=%s size=%dx%d frames=%d output=%s deadline=%" PRId64 "s\n",
            options.codec, options.width, options.height, options.frameCount, options.outputPath,
            options.deadlineSec);

    deadline = monotonicNanoseconds() + options.deadlineSec * 1000000000LL;
    const int minFrames =
            options.frameCount > 2 ? (options.frameCount - 2) : options.frameCount;
    while (!outputEOS && monotonicNanoseconds() < deadline) {
        if (!inputEOS && queuedFrames < options.frameCount) {
            ssize_t inputIndex = AMediaCodec_dequeueInputBuffer(codec, kDequeueTimeoutUs);
            if (inputIndex >= 0) {
                size_t capacity = 0;
                uint8_t *buffer =
                        AMediaCodec_getInputBuffer(codec, (size_t)inputIndex, &capacity);
                if (buffer == NULL || capacity == 0) {
                    result = failMessage("input-buffer", "empty-input-buffer");
                    goto cleanup;
                }
                fillNV12Frame(buffer, capacity, options.width, options.height, queuedFrames);
                const size_t frameSize =
                        (size_t)options.width * (size_t)options.height * 3U / 2U;
                const int64_t presentationTimeUs =
                        (int64_t)queuedFrames * 1000000LL / options.frameRate;
                status = AMediaCodec_queueInputBuffer(codec, (size_t)inputIndex, 0, frameSize,
                                                      presentationTimeUs, 0);
                if (status != AMEDIA_OK) {
                    result = failStatus("queue-input", status);
                    goto cleanup;
                }
                ++queuedFrames;
            }
        } else if (!inputEOS && queuedFrames >= options.frameCount) {
            ssize_t inputIndex = AMediaCodec_dequeueInputBuffer(codec, kDequeueTimeoutUs);
            if (inputIndex >= 0) {
                status = AMediaCodec_queueInputBuffer(codec, (size_t)inputIndex, 0, 0, 0,
                                                      AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
                if (status != AMEDIA_OK) {
                    result = failStatus("queue-input-eos", status);
                    goto cleanup;
                }
                inputEOS = true;
            }
        }

        AMediaCodecBufferInfo outputInfo;
        ssize_t outputIndex = AMediaCodec_dequeueOutputBuffer(codec, &outputInfo, kDequeueTimeoutUs);
        if (outputIndex >= 0) {
            ++outputBuffersSeen;
            size_t capacity = 0;
            uint8_t *buffer =
                    AMediaCodec_getOutputBuffer(codec, (size_t)outputIndex, &capacity);
            if (buffer == NULL) {
                result = failMessage("output-buffer", "empty-output-buffer");
                goto cleanup;
            }
            if (outputInfo.size > 0) {
                if ((size_t)outputInfo.offset + (size_t)outputInfo.size > capacity) {
                    result = failMessage("output-buffer", "output-range-out-of-bounds");
                    goto cleanup;
                }
                const uint8_t *payload = buffer + outputInfo.offset;
                if (write(outputFile, payload, (size_t)outputInfo.size) !=
                    (ssize_t)outputInfo.size) {
                    result = failMessage("write", "output-write-failed");
                    goto cleanup;
                }
                outputBytes += (size_t)outputInfo.size;
            }
            if ((outputInfo.flags & AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG) != 0) {
                ++codecConfigFrames;
            } else if (outputInfo.size > 0) {
                ++encodedFrames;
            }
            const bool annexB = outputInfo.size > 0 && looksLikeAnnexBStart(
                                                              buffer + outputInfo.offset,
                                                              (size_t)outputInfo.size);
            fprintf(stderr,
                    "output index=%zd size=%d flags=0x%x pts=%" PRId64 " encoded=%d annexb=%d\n",
                    outputIndex, outputInfo.size, outputInfo.flags, outputInfo.presentationTimeUs,
                    encodedFrames, annexB ? 1 : 0);
            status = AMediaCodec_releaseOutputBuffer(codec, (size_t)outputIndex, false);
            if (status != AMEDIA_OK) {
                result = failStatus("release-output", status);
                goto cleanup;
            }
            if ((outputInfo.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) != 0) {
                outputEOS = true;
            }
        } else if (isOutputFormatChanged(outputIndex)) {
            AMediaFormat *newFormat = AMediaCodec_getOutputFormat(codec);
            if (newFormat != NULL) {
                AMediaFormat_delete(newFormat);
            }
            ++formatChanges;
            fprintf(stderr, "output format-changed count=%d\n", formatChanges);
        } else if (isOutputBuffersChanged(outputIndex)) {
            fprintf(stderr, "output buffers-changed\n");
        } else if (isTryAgainLater(outputIndex)) {
            if (inputEOS && encodedFrames >= minFrames && formatChanges > 0) {
                break;
            }
        } else {
            fprintf(stderr, "dequeue-output unexpected index=%zd\n", outputIndex);
            result = failMessage("dequeue-output", "unexpected-index");
            goto cleanup;
        }
    }

    fprintf(stderr,
            "summary queued=%d encoded=%d codec_config=%d seen=%d format_changes=%d "
            "output_bytes=%zu eos=%d\n",
            queuedFrames, encodedFrames, codecConfigFrames, outputBuffersSeen, formatChanges,
            outputBytes, outputEOS ? 1 : 0);

    if (queuedFrames < options.frameCount) {
        result = failMessage("encode", "incomplete-input-queue");
        goto cleanup;
    }
    if (encodedFrames <= 0 || outputBytes == 0) {
        result = failMessage("encode", "no-encoded-output");
        goto cleanup;
    }
    if (!outputEOS) {
        if (encodedFrames >= minFrames && formatChanges > 0) {
            fprintf(stderr,
                    "note: accepting hard-encode without output EOS "
                    "(encoded=%d queued=%d format_changes=%d output_bytes=%zu)\n",
                    encodedFrames, queuedFrames, formatChanges, outputBytes);
        } else {
            result = failMessage("encode", "incomplete-or-timeout-before-output-eos");
            goto cleanup;
        }
    }

    printf("result=ok codec=%s mime=%s width=%d height=%d queued_frames=%d "
           "encoded_frames=%d format_changes=%d output_bytes=%zu eos=%d\n",
           options.codec, kExpectedMIME, options.width, options.height, queuedFrames,
           encodedFrames, formatChanges, outputBytes, outputEOS ? 1 : 0);
    result = 0;

cleanup:
    if (codec != NULL) {
        AMediaCodec_stop(codec);
        AMediaCodec_delete(codec);
    }
    if (format != NULL) {
        AMediaFormat_delete(format);
    }
    if (outputFile >= 0) {
        fsync(outputFile);
        close(outputFile);
    }
    return result;
}
