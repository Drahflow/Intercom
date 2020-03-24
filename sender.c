#include <pulse/pulseaudio.h>
#include <stdio.h>

#define BUFFER_SIZE 800

int running;

pa_context *ctx;
pa_stream *stream;

void streamStateChanged(pa_stream *_, void *__) {
  _ = __ = _;
  pa_stream_state_t state = pa_context_get_state(ctx);
  printf("pulseaudio stream state changed: %d\n", state);
}

void contextStateChanged(pa_context *_, void *__) {
  _ = __ = _;
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
  argc = argv + argc - argv;

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
