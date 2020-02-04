#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <pulse/error.h>
#include <pulse/context.h>
#include <pulse/subscribe.h>
#include <pulse/introspect.h>
#include <pulse/mainloop.h>
#include <pulse/mainloop-api.h>

// No idea what the actual bound on this is
#define SINK_NAME_MAX 256

typedef struct app {
    // Reference to the mainloop, for quitting
    pa_mainloop *mainloop;

    // Current default sink
    char *default_sink;

    // Tracks if we've started subscribing to events yet or not. Used in server_info_callback to defer subscribing
    // until we know the default sink
    int is_subscribed;
} app_t;

/*
 * PulseAudio context state callback. When the context is ready, subscribes to receive events. 
 */
void state_callback(pa_context *context, void *userdata);

/*
 * Success callback which quits if the operation failed
 */
void success_callback(pa_context *context, int success, void *userdata);

/*
 * Subscription event callback. Receives events from PulseAudio when objects change.
 */
void subscription_event_callback(pa_context *context, pa_subscription_event_type_t event, uint32_t index, void *userdata);

/*
 * Server information callback. Updates the default sink based on what the server specifies
 */
void server_info_callback(pa_context *context, const pa_server_info *info, void *userdata);

/*
 * Callback for sink information. Prints out the volume and whether or not the sink is muted. The assumption is that
 * we can be verbose here and consumers either can deduplicate or are doing something idempotent with the information
 * like update a volume indicator.
 */
void sink_info_callback(pa_context *context, const pa_sink_info *info, int eol, void *userdata);

int main() {
    int status = 0;
    pa_mainloop *mainloop = pa_mainloop_new();
    app_t app = {
        .mainloop = mainloop,
        .default_sink = NULL,
        .is_subscribed = 0,
    };

    // TODO: specify proplist
    pa_context *context = pa_context_new(pa_mainloop_get_api(mainloop), "pa-volume-watcher");
    pa_context_set_state_callback(context, state_callback, &app);
    int err = pa_context_connect(context, NULL, 0, NULL);
    if (err != PA_OK) {
        fprintf(stderr, "Could not connect to server: %s\n", pa_strerror(err));
        status = 1;
        goto cleanup;
    }

    err = pa_mainloop_run(mainloop, &status);
    if (err != PA_OK) {
        fprintf(stderr, "PulseAudio error: %s\n", pa_strerror(err));
        status = 1;
    }

cleanup:
    pa_mainloop_free(mainloop);
    return status;
}

void state_callback(pa_context *context, void *userdata) {
    app_t *app = (app_t*) userdata;

    switch (pa_context_get_state(context))
    {
        case PA_CONTEXT_READY:
            // Request server info to set the default sink name. server_info_callback will then start subscribing
            // to events.
            pa_context_get_server_info(context, server_info_callback, app);
            break;
        case PA_CONTEXT_FAILED:
            fprintf(stderr, "PulseAudio connection failed\n");
            pa_mainloop_quit(app->mainloop, 1);
            break;
        default:
            break;
    }
}

void subscription_event_callback(pa_context *context, pa_subscription_event_type_t event, uint32_t index, void *userdata) {
    app_t *app = (app_t *) userdata;

    // We only care about change events
    if ((event & PA_SUBSCRIPTION_EVENT_TYPE_MASK) == PA_SUBSCRIPTION_EVENT_CHANGE) {
        switch (event & PA_SUBSCRIPTION_EVENT_FACILITY_MASK) {
            case PA_SUBSCRIPTION_EVENT_SERVER:
                // _Something_ in the server changed, so update our view of the defaults
                pa_context_get_server_info(context, server_info_callback, app);
                break;
            case PA_SUBSCRIPTION_EVENT_SINK:
                // A sink changed, so check the volume. This means we'll generate output even if sinks other than
                // the default changed, but that should be fine
                if (app->default_sink != NULL) {
                    pa_context_get_sink_info_by_name(context, app->default_sink, sink_info_callback, app);
                }
            default:
                break;
        }
    }
}

void server_info_callback(pa_context *context, const pa_server_info *info, void *userdata) {
    app_t *app = (app_t *) userdata;

    if (app->default_sink != NULL && strncmp(app->default_sink, info->default_sink_name, SINK_NAME_MAX) == 0) {
        // Default sink did not change, ignore
        return;
    }

    if (app->default_sink != NULL) {
        free(app->default_sink);
    }

    size_t buf_size = strlen(info->default_sink_name) + 1;

    app->default_sink = malloc(buf_size);
    if (app->default_sink == NULL) {
        fprintf(stderr, "Could not allocate memory for sink name\n");
        pa_mainloop_quit(app->mainloop, 1);
        return;
    }

    memcpy(app->default_sink, info->default_sink_name, buf_size);
    // Check if this is the initial server_info call from when we first connect. If so, we need to start subscribing to events
    if (!app->is_subscribed) {
        app->is_subscribed = 1;
        pa_context_set_subscribe_callback(context, subscription_event_callback, app);
        pa_context_subscribe(context, PA_SUBSCRIPTION_MASK_SINK | PA_SUBSCRIPTION_MASK_SERVER, success_callback, app);
    }

    // If we changed sinks, we'll need to output the volume on the new sink
    pa_context_get_sink_info_by_name(context, app->default_sink, sink_info_callback, app);
}

void sink_info_callback(pa_context *context, const pa_sink_info *info, int eol, void *userdata) {
    if (eol) return;

    app_t *app = (app_t *) userdata;

    if (strncmp(app->default_sink, info->name, SINK_NAME_MAX) != 0) {
        // Somehow got info for the wrong sink. Probably a race condition where the default sink changed and we found
        // out before a pending sink info operation completed
        return;
    }

    // See https://freedesktop.org/software/pulseaudio/doxygen/volume.html
    // The volume will be on PulseAudio's cubic scale, but since we're not performing calculations on it I think we
    // can still treat it like a percent between muted and normal. Based on the docs, the volume conversion functions
    // are likely not valid here anyways
    pa_volume_t volume = pa_cvolume_avg(&info->volume);

    // Taken from pavucontrol's channel widget: https://github.com/pulseaudio/pavucontrol/blob/master/src/channelwidget.cc
    double volume_percent = ((double) volume * 100) / PA_VOLUME_NORM;
    printf("volume = %0.f muted = %d\n", volume_percent, info->mute);
    fflush(stdout); // Ensure consumer sees update
}

void success_callback(pa_context *context, int success, void *userdata) {
    if (!success) {
        app_t *app = (app_t *) userdata;
        pa_mainloop_quit(app->mainloop, 1);
    }
}