/*
 * osynth host port — MIDI input, over RtMidi.
 *
 * Opens the host's MIDI input ports and feeds what arrives into the firmware's
 * own router. Nothing downstream knows the difference: midi.c's
 * midi_route_channel_message() is the same entry point the USB and DIN inputs
 * use on the instrument, so chord mode, the arpeggiator, the sequencer's
 * recorder, the mod matrix's wheel source and per-engine CC maps all work
 * from a host keyboard exactly as they do from a hardware one.
 *
 * ---------------------------------------------------------------------------
 * Every port, not a chosen one
 *
 * A hardware synth has one DIN socket and one USB port. A computer has a list
 * that changes while the program runs, and the player's keyboard may be any
 * entry in it -- or may be plugged in after the app starts. So this opens
 * every input port it finds and merges them, which is what makes "plug in a
 * keyboard and play" work with no configuration.
 *
 * The cost is that a port carrying something unwanted is also merged. That is
 * the right default for an instrument: the alternative is a device picker in
 * the way of the first note. If a chooser is ever wanted, it belongs in the
 * app's settings and should call osynth_host_midi_in_open() with an index.
 *
 * ---------------------------------------------------------------------------
 * Threading
 *
 * RtMidi delivers on its own thread, one callback per message. The router is
 * documented safe from any control task -- its note path is the same lock-free
 * ring every other control task pushes through -- so messages go straight in
 * rather than through a queue of our own.
 */
#include "osynth_host_midi.h"

#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "esp_log.h"

#include "midi.h"
#include "synth_config.h"

#if OSYNTH_HOST_MIDI_IN
#include "RtMidi.h"
#endif

namespace {

const char* TAG = "midi_in";

#if OSYNTH_HOST_MIDI_IN

std::mutex g_mutex;
std::vector<std::unique_ptr<RtMidiIn>> g_ports;

void on_message(double /*delta*/, std::vector<unsigned char>* msg,
                void* /*ctx*/) {
    if (msg == nullptr || msg->empty()) return;
    const unsigned char status = (*msg)[0];

    /* System Real-Time (0xF8..0xFF). One byte, and it may arrive *inside*
     * another message on a real cable, which is why it is checked first and
     * separately. Clock, start, continue and stop drive the sequencer; the
     * router ignores the rest. */
    if (status >= 0xF8) {
        midi_route_realtime(status);
        return;
    }

    /* Channel-voice messages (0x8n..0xEn). System Common (0xF0..0xF7) is
     * dropped: SysEx is not part of SynthCtl and the router has no entry point
     * for it. */
    if (status < 0x80 || status >= 0xF0) return;

    const unsigned char d1 = msg->size() > 1 ? (*msg)[1] : 0;
    /* d2 = 0 for the two-byte messages (program change, channel pressure) --
     * the contract midi.h states for this call. */
    const unsigned char d2 = msg->size() > 2 ? (*msg)[2] : 0;
    midi_route_channel_message(status, d1, d2);
}

#endif /* OSYNTH_HOST_MIDI_IN */

}  // namespace

esp_err_t osynth_host_midi_in_start(void) {
#if !OSYNTH_HOST_MIDI_IN
    ESP_LOGI(TAG, "no MIDI backend on this platform");
    return ESP_ERR_NOT_SUPPORTED;
#else
    std::lock_guard<std::mutex> lk(g_mutex);
    if (!g_ports.empty()) return ESP_OK;

    unsigned int count = 0;
    try {
        RtMidiIn probe;
        count = probe.getPortCount();
    } catch (RtMidiError& e) {
        /* RtMidi reports by exception; the rest of this tree does not, so the
         * boundary is here. A machine with no MIDI stack at all is not an
         * error -- the synth plays from the app's keyboard regardless. */
        ESP_LOGW(TAG, "MIDI unavailable: %s", e.getMessage().c_str());
        return ESP_OK;
    }

    if (count == 0) {
        ESP_LOGI(TAG, "no MIDI input ports");
        return ESP_OK;
    }

    for (unsigned int i = 0; i < count; ++i) {
        try {
            auto in = std::make_unique<RtMidiIn>();
            const std::string name = in->getPortName(i);
            in->openPort(i, "osynth");
            /* Timing bytes are the sequencer's clock, so they must not be
             * filtered. SysEx and active sensing are: nothing here consumes
             * the first, and the second is pure traffic. */
            in->ignoreTypes(/*sysex=*/true, /*timing=*/false,
                            /*sensing=*/true);
            in->setCallback(&on_message);
            ESP_LOGI(TAG, "input %u: %s", i, name.c_str());
            g_ports.push_back(std::move(in));
        } catch (RtMidiError& e) {
            /* One port refusing to open -- taken exclusively by another
             * program, most often -- must not cost the others. */
            ESP_LOGW(TAG, "input %u could not be opened: %s", i,
                     e.getMessage().c_str());
        }
    }

    ESP_LOGI(TAG, "up: %u of %u port(s) open, merged into the router",
             (unsigned)g_ports.size(), count);
    return ESP_OK;
#endif
}

void osynth_host_midi_in_stop(void) {
#if OSYNTH_HOST_MIDI_IN
    std::lock_guard<std::mutex> lk(g_mutex);
    /* Cancelling the callback before the object goes is what guarantees no
     * message is delivered into a half-destroyed port. */
    for (auto& p : g_ports) {
        if (p) p->cancelCallback();
    }
    g_ports.clear();
#endif
}

int osynth_host_midi_in_ports(void) {
#if OSYNTH_HOST_MIDI_IN
    std::lock_guard<std::mutex> lk(g_mutex);
    return (int)g_ports.size();
#else
    return 0;
#endif
}
