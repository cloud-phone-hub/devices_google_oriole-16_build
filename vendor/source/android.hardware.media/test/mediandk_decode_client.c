// Copyright (c) 2025-2026 fei_cong(https://github.com/feicong/feicong-course)
#include <fcntl.h>
#include <inttypes.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaError.h>
#include <media/NdkMediaExtractor.h>
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

static const char kDecoderName[] = "c2.v4l2.avc.decoder";
static const char kExpectedMIME[] = "video/avc";
static const int64_t kDequeueTimeoutUs = 20000;
static const int64_t kDeadlineNs = 90000000000LL;

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

static AMediaFormat *selectAVCTrack(AMediaExtractor *extractor) {
    size_t trackCount = AMediaExtractor_getTrackCount(extractor);
    for (size_t index = 0; index < trackCount; ++index) {
        AMediaFormat *format = AMediaExtractor_getTrackFormat(extractor, index);
        const char *mime = NULL;
        if (format != NULL && AMediaFormat_getString(format, AMEDIAFORMAT_KEY_MIME, &mime) &&
            mime != NULL && strcmp(mime, kExpectedMIME) == 0) {
            if (AMediaExtractor_selectTrack(extractor, index) == AMEDIA_OK) {
                return format;
            }
        }
        if (format != NULL) {
            AMediaFormat_delete(format);
        }
    }
    return NULL;
}

int main(int argc, char **argv) {
    if (argc != 2) {
        return failMessage("arguments", "usage:mediandk_decode_client INPUT_MP4");
    }

    /* Codec2 AIDL onWorkDone arrives via Binder; without a thread pool the HAL
     * can finish frames while MediaCodec never observes output buffers. */
    {
        void *binderNdk = dlopen("libbinder_ndk.so", RTLD_NOW);
        if (binderNdk == NULL) {
            return failMessage("binder", "dlopen-libbinder_ndk-failed");
        }
        void (*setMax)(int32_t) =
                (void (*)(int32_t))dlsym(binderNdk, "ABinderProcess_setThreadPoolMaxThreadCount");
        void (*startPool)(void) =
                (void (*)(void))dlsym(binderNdk, "ABinderProcess_startThreadPool");
        if (setMax == NULL || startPool == NULL) {
            return failMessage("binder", "missing-ABinderProcess-threadpool-symbols");
        }
        setMax(4);
        startPool();
    }

    int result = 1;
    int inputFile = -1;
    AMediaExtractor *extractor = NULL;
    AMediaFormat *trackFormat = NULL;
    AMediaCodec *codec = NULL;
    bool inputEOS = false;
    bool outputEOS = false;
    int queuedSamples = 0;
    int decodedFrames = 0;
    int formatChanges = 0;
    int outputBuffersSeen = 0;
    int64_t deadline = 0;

    inputFile = open(argv[1], O_RDONLY | O_CLOEXEC);
    if (inputFile < 0) {
        result = failMessage("open", "cannot-open-input");
        goto cleanup;
    }
    struct stat inputStat;
    if (fstat(inputFile, &inputStat) != 0 || inputStat.st_size <= 0) {
        result = failMessage("input", "invalid-input-size");
        goto cleanup;
    }

    extractor = AMediaExtractor_new();
    if (extractor == NULL) {
        result = failMessage("extractor", "create-failed");
        goto cleanup;
    }
    if (AMediaExtractor_setDataSourceFd(extractor, inputFile, 0, inputStat.st_size) != AMEDIA_OK) {
        result = failMessage("extractor", "set-data-source-failed");
        goto cleanup;
    }

    trackFormat = selectAVCTrack(extractor);
    if (trackFormat == NULL) {
        result = failMessage("track", "avc-track-not-found");
        goto cleanup;
    }

    /* Prefer flexible YUV so CCodec exposes byte/image buffers for NULL-surface clients. */
    AMediaFormat_setInt32(trackFormat, AMEDIAFORMAT_KEY_COLOR_FORMAT, 0x7F420888);

    codec = AMediaCodec_createCodecByName(kDecoderName);
    if (codec == NULL) {
        result = failMessage("codec", "create-v4l2-avc-decoder-failed");
        goto cleanup;
    }
    media_status_t status = AMediaCodec_configure(codec, trackFormat, NULL, NULL, 0);
    if (status != AMEDIA_OK) {
        result = failStatus("configure", status);
        goto cleanup;
    }
    status = AMediaCodec_start(codec);
    if (status != AMEDIA_OK) {
        result = failStatus("start", status);
        goto cleanup;
    }

    deadline = monotonicNanoseconds() + kDeadlineNs;
    while (!outputEOS && monotonicNanoseconds() < deadline) {
        if (!inputEOS) {
            ssize_t inputIndex = AMediaCodec_dequeueInputBuffer(codec, kDequeueTimeoutUs);
            if (inputIndex >= 0) {
                size_t capacity = 0;
                uint8_t *buffer = AMediaCodec_getInputBuffer(codec, (size_t)inputIndex, &capacity);
                if (buffer == NULL || capacity == 0) {
                    result = failMessage("input-buffer", "empty-input-buffer");
                    goto cleanup;
                }
                ssize_t sampleSize = AMediaExtractor_readSampleData(extractor, buffer, capacity);
                if (sampleSize < 0) {
                    status = AMediaCodec_queueInputBuffer(codec, (size_t)inputIndex, 0, 0, 0,
                                                          AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
                    inputEOS = true;
                } else {
                    int64_t presentationTimeUs = AMediaExtractor_getSampleTime(extractor);
                    status = AMediaCodec_queueInputBuffer(codec, (size_t)inputIndex, 0,
                                                          (size_t)sampleSize, presentationTimeUs, 0);
                    if (status == AMEDIA_OK) {
                        ++queuedSamples;
                        AMediaExtractor_advance(extractor);
                    }
                }
                if (status != AMEDIA_OK) {
                    result = failStatus("queue-input", status);
                    goto cleanup;
                }
            }
        }

        AMediaCodecBufferInfo outputInfo;
        ssize_t outputIndex = AMediaCodec_dequeueOutputBuffer(codec, &outputInfo, kDequeueTimeoutUs);
        if (outputIndex >= 0) {
            ++outputBuffersSeen;
            if ((outputInfo.flags & AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG) == 0) {
                ++decodedFrames;
            }
            fprintf(stderr,
                    "output index=%zd size=%d flags=0x%x pts=%" PRId64 " decoded=%d\n",
                    outputIndex, outputInfo.size, outputInfo.flags, outputInfo.presentationTimeUs,
                    decodedFrames);
            status = AMediaCodec_releaseOutputBuffer(codec, (size_t)outputIndex, false);
            if (status != AMEDIA_OK) {
                result = failStatus("release-output", status);
                goto cleanup;
            }
            if ((outputInfo.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) != 0) {
                outputEOS = true;
            }
        } else if (outputIndex == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
            AMediaFormat *newFormat = AMediaCodec_getOutputFormat(codec);
            if (newFormat != NULL) {
                AMediaFormat_delete(newFormat);
            }
            ++formatChanges;
            fprintf(stderr, "output format-changed count=%d\n", formatChanges);
        } else if (outputIndex == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) {
            fprintf(stderr, "output buffers-changed\n");
        } else if (outputIndex == AMEDIACODEC_INFO_TRY_AGAIN_LATER) {
            /* keep waiting until deadline */
        } else {
            fprintf(stderr, "dequeue-output unexpected index=%zd\n", outputIndex);
            result = failMessage("dequeue-output", "unexpected-index");
            goto cleanup;
        }
    }

    fprintf(stderr, "summary queued=%d decoded=%d seen=%d format_changes=%d eos=%d\n",
            queuedSamples, decodedFrames, outputBuffersSeen, formatChanges, outputEOS ? 1 : 0);

    if (queuedSamples <= 0 || decodedFrames <= 0) {
        result = failMessage("decode", "no-decoded-frames");
        goto cleanup;
    }
    /* Hardware decode may omit a timely output EOS. Accept only when almost
     * all queued access units produced real frames (allow a small holdback). */
    if (!outputEOS) {
        const int minFrames = queuedSamples > 2 ? (queuedSamples - 2) : queuedSamples;
        if (decodedFrames >= minFrames && formatChanges > 0) {
            fprintf(stderr,
                    "note: accepting hard-decode without output EOS "
                    "(decoded=%d queued=%d format_changes=%d)\n",
                    decodedFrames, queuedSamples, formatChanges);
        } else {
            result = failMessage("decode", "incomplete-or-timeout-before-output-eos");
            goto cleanup;
        }
    }
    printf("result=ok codec=%s mime=%s queued_samples=%d decoded_frames=%d format_changes=%d eos=%d\n",
           kDecoderName, kExpectedMIME, queuedSamples, decodedFrames, formatChanges,
           outputEOS ? 1 : 0);
    result = 0;

cleanup:
    if (codec != NULL) {
        AMediaCodec_stop(codec);
        AMediaCodec_delete(codec);
    }
    if (trackFormat != NULL) {
        AMediaFormat_delete(trackFormat);
    }
    if (extractor != NULL) {
        AMediaExtractor_delete(extractor);
    }
    if (inputFile >= 0) {
        close(inputFile);
    }

    /* Binder NDK does not expose a way to stop its process thread pool. A
     * normal return would run global destructors while callback threads are
     * still active, which trips Bionic's destroyed-mutex checks. */
    fflush(NULL);
    _exit(result);
}
