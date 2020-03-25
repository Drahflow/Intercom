#include "common.h"

#include <pulse/pulseaudio.h>
#include <stdio.h>
#include <time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>

#define BUFFER_SIZE 400
#define IGN(x) __##x __attribute__((unused))

int running;
float initialSampleRate = 8000;
float sampleRate;
float sampleRateBlend = 0.0005;
float localPositionAvg;
float localPositionBlend = 0.05;

double targetLatency = 0.05;  // in s
uint64_t senderOffset; // incoming packet offset which would start at receiveBuffer[0]
char receiveBuffer[8000];

pa_context *ctx;
pa_stream *stream;
int udpSocket;

int streamReady = 0;

void streamStateChanged(pa_stream *IGN(stream), void *IGN(userdata)) {
  pa_stream_state_t state = pa_stream_get_state(stream);
  printf("pulseaudio stream state changed: %d\n", state);

  if(state != PA_STREAM_READY) return;

  streamReady = 1;
}

void contextStateChanged(pa_context *IGN(ctx), void *IGN(userdata)) {
  pa_context_state_t state = pa_context_get_state(ctx);
  printf("pulseaudio context state changed: %d\n", state);

  if(state != PA_CONTEXT_READY) return;

  pa_sample_spec sample_spec;
  sample_spec.format = PA_SAMPLE_ALAW;
  sample_spec.channels = 1;
  sample_spec.rate = sampleRate;

  stream = pa_stream_new(ctx, "intercom-output", &sample_spec, NULL);
  if(!stream) {
    fprintf(stderr, "Failed to create pulseaudio stream: %s\n", pa_strerror(pa_context_errno(ctx)));
    running = 0;
    return;
  }

  pa_stream_set_state_callback(stream, streamStateChanged, NULL);
  // pa_stream_set_write_callback(stream, writeRequested, NULL);

  pa_buffer_attr buffer_spec;
  buffer_spec.maxlength = ~0u;
  buffer_spec.tlength = BUFFER_SIZE;
  buffer_spec.prebuf = ~0u;
  buffer_spec.minreq = ~0u;
  
  if(pa_stream_connect_playback(stream, NULL, &buffer_spec, PA_STREAM_PLAYBACK | PA_STREAM_ADJUST_LATENCY | PA_STREAM_NOT_MONOTONIC | PA_STREAM_VARIABLE_RATE, NULL, NULL)) {
    fprintf(stderr, "Failed to connect playback stream: %s\n", pa_strerror(pa_context_errno(ctx)));
    running = 0;
    return;
  }
}

void receiveNetwork() {
  while(1) {
    networkPacket packet;

    int len = recv(udpSocket, &packet, sizeof(packet), MSG_DONTWAIT);
    if(len < 0) {
      if(errno == EAGAIN || errno == EWOULDBLOCK) return;

      fprintf(stderr, "Failed to receive packet: %s\n", strerror(errno));
      return;
    }

    struct timespec t;
    if(clock_gettime(CLOCK_REALTIME, &t)) {
      fprintf(stderr, "Failed to get current time: %s\n", strerror(errno));
      return;
    }

    uint64_t now = (uint64_t)(t.tv_sec) * 1000000000 + t.tv_nsec;
    double packetToPlayIn = (packet.time + targetLatency * 1000000000 - now) / 1000000000;

    int dataLen = len - sizeof(packet) + sizeof(packet.data);
    int64_t localPosition = packet.position - senderOffset;

    printf("Packet would need to play in: %lf, local position: %lld\n", packetToPlayIn, (long long int)localPosition);

    if(packetToPlayIn < 0) {
      fprintf(stderr, "Packet arrived too late.\n");
    } else if(localPosition < 0) {
      fprintf(stderr, "Playback is too far ahead.\n");

      bzero(receiveBuffer, sizeof(receiveBuffer));
      senderOffset = packet.position - sizeof(receiveBuffer) / sampleRate * targetLatency;
      localPositionAvg = localPosition = packet.position - senderOffset;
      sampleRate = initialSampleRate;
    } else if(localPosition + dataLen > (int)sizeof(receiveBuffer)) {
      fprintf(stderr, "Playback is too far behind.\n");

      bzero(receiveBuffer, sizeof(receiveBuffer));
      senderOffset = packet.position - sizeof(receiveBuffer) / sampleRate * targetLatency;
      localPositionAvg = localPosition = packet.position - senderOffset;
      sampleRate = initialSampleRate;
    } else {
      memcpy(receiveBuffer + localPosition, packet.data, dataLen);

      float onTargetSampleRate = localPosition / packetToPlayIn;

      localPositionAvg = (1 - localPositionBlend) * localPositionAvg + localPositionBlend * localPosition;

      printf("Received packet, local position %lld. onTargetSampleRate would be %f\n", (long long int)localPosition, onTargetSampleRate);

      float newSampleRate = (1 - sampleRateBlend) * sampleRate + sampleRateBlend * onTargetSampleRate;

      if(newSampleRate < sampleRate && localPosition < localPositionAvg) {
        sampleRate = newSampleRate;
      } else if(newSampleRate > sampleRate && localPosition > localPositionAvg) {
        sampleRate = newSampleRate;
      }
    }

    if(sampleRate > 4000 && sampleRate < 12000) {
      if(streamReady) {
        printf("Setting new sample rate: %d\n", (uint32_t)sampleRate);
        pa_stream_update_sample_rate(stream, sampleRate, NULL, NULL);
      } else {
        printf("Stream is not ready, yet.\n");
      }
    }
  }
}

void writeAudio() {
  if(!streamReady) return;

  size_t requested = pa_stream_writable_size(stream);
  if(!requested) return;

  if(pa_stream_is_corked(stream)) {
    pa_stream_cork(stream, 0, NULL, NULL);
  }
    
  if(pa_stream_write(stream, receiveBuffer, requested, NULL, 0, PA_SEEK_RELATIVE)) {
    fprintf(stderr, "Could not write to pulseaudio stream: %s\n", pa_strerror(pa_context_errno(ctx)));
    return;
  }

  memmove(receiveBuffer, receiveBuffer + requested, sizeof(receiveBuffer) - requested);
  bzero(receiveBuffer + sizeof(receiveBuffer) - requested, requested);
  senderOffset += requested;

  printf("Played %lld samples.\n", (long long int)requested);
  fprintf(stderr, "%s\n", pa_strerror(pa_context_errno(ctx)));
  fprintf(stderr, "%d %d\n", pa_stream_is_corked(stream), pa_stream_is_suspended(stream));
}

int main(int argc, char **argv) {
  if(argc != 2) {
    fprintf(stderr, "Usage: ./receiver <target port>\n");
    return 1;
  }

  struct addrinfo *listenAddr;

  struct addrinfo hints;
  hints.ai_flags = AI_ADDRCONFIG | AI_PASSIVE; // Don't resolve for IP protocols the host has no own address for.
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_DGRAM;
  hints.ai_protocol = 0; // Anything packet will do.
  hints.ai_canonname = NULL;
  hints.ai_addr = NULL;
  hints.ai_next = NULL;

  int gaiError = getaddrinfo(NULL, argv[1], &hints, &listenAddr);
  if(gaiError) {
    fprintf(stderr, "Could not resolve listening address: %s\n", gai_strerror(gaiError));
    return 1;
  }

  senderOffset = -1ull << 62;
  sampleRate = initialSampleRate;

  udpSocket = socket(listenAddr->ai_family, SOCK_DGRAM, 0);
  if(udpSocket < 0) {
    fprintf(stderr, "Could not create UDP socket: %s\n", strerror(errno));
    return 1;
  }

  if(bind(udpSocket, listenAddr->ai_addr, listenAddr->ai_addrlen)) {
    fprintf(stderr, "Could not set listening address on UDP socket: %s\n", strerror(errno));
    return 1;
  }

  pa_mainloop *mainloop = pa_mainloop_new();
  if(!mainloop) {
    fprintf(stderr, "Failed to get pulseaudio mainloop.\n");
    return 1;
  }

  ctx = pa_context_new(
    pa_mainloop_get_api(mainloop),
    "Intercom"
  );
  if(!ctx) {
    fprintf(stderr, "Failed to get pulseaudio context.\n");
    return 1;
  }

  pa_context_set_state_callback(ctx, contextStateChanged, NULL);

  if(pa_context_connect(ctx, NULL, PA_CONTEXT_NOFLAGS, NULL)) {
    fprintf(stderr, "Failed to connect pulseaudio context: %s\n", pa_strerror(pa_context_errno(ctx))); 
    return 1;
  }

  running = 1;

  while(running) {
    pa_mainloop_iterate(mainloop, 0, NULL);

    writeAudio();
    receiveNetwork();

    usleep(5000);
  }

  return 0;
}
