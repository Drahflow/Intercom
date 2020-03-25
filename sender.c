#include "common.h"

#include <pulse/pulseaudio.h>
#include <stdio.h>
#include <time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <errno.h>
#include <string.h>

#define BUFFER_SIZE 800
#define IGN(x) __##x __attribute__((unused))

int running;

pa_context *ctx;
pa_stream *stream;
uint64_t position;
int udpSocket;

void streamStateChanged(pa_stream *IGN(stream), void *IGN(userdata)) {
  pa_stream_state_t state = pa_context_get_state(ctx);
  printf("pulseaudio stream state changed: %d\n", state);
}

void dataAvailable(pa_stream *IGN(stream), size_t IGN(bytes), void *IGN(userdata)) {
  size_t available;
  const void *data;

  if(pa_stream_peek(stream, &data, &available)) {
    fprintf(stderr, "Failed to read data from stream: %s\n", pa_strerror(pa_context_errno(ctx)));
    return;
  }

  struct timespec t;
  if(clock_gettime(CLOCK_REALTIME, &t)) {
    fprintf(stderr, "Failed to get current time: %s\n", strerror(errno));
    return;
  }

  networkPacket packet;
  packet.position = position;
  packet.time = (uint64_t)(t.tv_sec) * 1000000000 + t.tv_nsec;
  memcpy(packet.data, data, available);

  send(udpSocket, &packet, sizeof(packet) - sizeof(packet.data) + available, 0);

  // In case of packet loss...
  send(udpSocket, &packet, sizeof(packet) - sizeof(packet.data) + available, 0);

  position += available;
  printf("Data transmitted. Position now at: %lu\n", position);

  if(pa_stream_drop(stream)) {
    fprintf(stderr, "Failed to acknowledge stream data: %s\n", pa_strerror(pa_context_errno(ctx)));
    return;
  }
}

void contextStateChanged(pa_context *IGN(ctx), void *IGN(userdata)) {
  pa_context_state_t state = pa_context_get_state(ctx);
  printf("pulseaudio context state changed: %d\n", state);

  if(state != PA_CONTEXT_READY) return;

  pa_sample_spec sample_spec;
  sample_spec.format = PA_SAMPLE_ALAW;
  sample_spec.channels = 1;
  sample_spec.rate = 8000;

  stream = pa_stream_new(ctx, "intercom-input", &sample_spec, NULL);
  if(!stream) {
    fprintf(stderr, "Failed to create pulseaudio stream: %s\n", pa_strerror(pa_context_errno(ctx)));
    running = 0;
    return;
  }

  pa_stream_set_state_callback(stream, streamStateChanged, NULL);
  pa_stream_set_read_callback(stream, dataAvailable, NULL);

  pa_buffer_attr buffer_spec;
  buffer_spec.maxlength = BUFFER_SIZE;
  buffer_spec.fragsize = BUFFER_SIZE;
  
  if(pa_stream_connect_record(stream, NULL, &buffer_spec, PA_STREAM_RECORD | PA_STREAM_ADJUST_LATENCY | PA_STREAM_NOT_MONOTONIC | PA_STREAM_VARIABLE_RATE)) {
    fprintf(stderr, "Failed to connect recording stream: %s\n", pa_strerror(pa_context_errno(ctx)));
    running = 0;
    return;
  }
}

int main(int argc, char **argv) {
  if(argc != 3) {
    fprintf(stderr, "Usage: ./sender <target host> <target port>\n");
    return 1;
  }

  struct addrinfo *peer;

  struct addrinfo hints;
  hints.ai_flags = AI_ADDRCONFIG; // Don't resolve for IP protocols the host has no own address for.
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_DGRAM;
  hints.ai_protocol = 0; // Anything packet will do.
  hints.ai_canonname = NULL;
  hints.ai_addr = NULL;
  hints.ai_next = NULL;

  int gaiError = getaddrinfo(argv[1], argv[2], &hints, &peer);
  if(gaiError) {
    fprintf(stderr, "Could not resolve peer address: %s\n", gai_strerror(gaiError));
    return 1;
  }
  position = 0;

  udpSocket = socket(peer->ai_family, SOCK_DGRAM, 0);
  if(udpSocket < 0) {
    fprintf(stderr, "Could not create UDP socket: %s\n", strerror(errno));
    return 1;
  }

  if(connect(udpSocket, peer->ai_addr, peer->ai_addrlen)) {
    fprintf(stderr, "Could not set target address on UDP socket: %s\n", strerror(errno));
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
    pa_mainloop_iterate(mainloop, 1, NULL);
  }

  return 0;
}
