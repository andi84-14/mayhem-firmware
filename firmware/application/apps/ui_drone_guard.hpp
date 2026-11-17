**
 * ============================================================================
 * TACTICAL DRONE GUARD & DIRECTION FINDER (für PortaPack H2 / Mayhem Firmware)
 * ============================================================================
 * 
 * DATEI-STRUKTUR FÜR DIE KOMPILIERUNG:
 * Speichere den ersten Teil als `ui_drone_guard.hpp` und den zweiten Teil als
 * `ui_drone_guard.cpp` im Ordner `firmware/application/apps/`, ODER binde diese
 * Datei direkt in das Menü (`ui_navigation.cpp`) ein (siehe Anleitung im Guide).
 */

#ifndef __UI_DRONE_GUARD_H__
#define __UI_DRONE_GUARD_H__

#include "ui.hpp"
#include "ui_widget.hpp"
#include "ui_navigation.hpp"
#include "ui_rssi.hpp"
#include "receiver_model.hpp"
#include "radio.hpp"
#include "audio.hpp"
#include "baseband_api.hpp"
#include "message.hpp"
#include "portapack.hpp"
#include "string_format.hpp"

#include <array>
#include <algorithm>

using namespace portapack;

namespace ui {

// Struktur für die zu überwachenden Drohnen-Kanäle (optimiert für 2.4 / 5.8 GHz Dual-Antenne)
struct DroneChannel {
    rf::Frequency freq;
    const char* label;
    const char* type;
};

// Die wichtigsten Frequenzen für Aufklärungs- und FPV-Drohnen im 2.4 & 5.8 GHz Band
static constexpr std::array<DroneChannel, 18> DRONE_CHANNELS = {{
    // 2.4 GHz Band (DJI OcuSync, ELRS, Wi-Fi Steuerung & Telemetrie)
    { 2412000000, "2.4G Ch1 (2412)",  "DJI / WiFi / ELRS" },
    { 2422000000, "2.4G Ch3 (2422)",  "DJI OcuSync / Mavic" },
    { 2437000000, "2.4G Ch6 (2437)",  "DJI / WiFi / Autel" },
    { 2452000000, "2.4G Ch9 (2452)",  "DJI OcuSync / ELRS" },
    { 2462000000, "2.4G Ch11 (2462)", "DJI / WiFi / Mavic" },
    { 2472000000, "2.4G Ch13 (2472)", "ELRS / FHSS Control" },

    // 5.8 GHz Band (Analoge FPV Video-Sender & Digitale HD-Links wie DJI/Walksnail)
    { 5658000000, "5.8G R1 (5658)",   "FPV Video Raceband 1" },
    { 5695000000, "5.8G R2 (5695)",   "FPV Video Raceband 2" },
    { 5732000000, "5.8G R3 (5732)",   "FPV Video Raceband 3" },
    { 5740000000, "5.8G F1 (5740)",   "FPV Fatshark / DJI" },
    { 5745000000, "5.8G SRD (5745)",  "DJI OcuSync 5.8G" },
    { 5769000000, "5.8G R4 (5769)",   "FPV Video Raceband 4" },
    { 5785000000, "5.8G SRD (5785)",  "DJI OcuSync / Autel" },
    { 5806000000, "5.8G R5 (5806)",   "FPV Video Raceband 5" },
    { 5825000000, "5.8G SRD (5825)",  "DJI OcuSync High" },
    { 5843000000, "5.8G R6 (5843)",   "FPV Video Raceband 6" },
    { 5860000000, "5.8G F7 (5860)",   "FPV Video Fatshark 7" },
    { 5880000000, "5.8G R7 (5880)",   "FPV Video Raceband 7" }
}};

class DroneGuardView : public View {
public:
    DroneGuardView(NavigationView& nav) : nav_(nav) {
        // Baseband für Spektrum/RSSI und Ton-Synthese starten
        baseband::run_image(portapack::spi_flash::image_tag_wideband_spectrum);

        add_children({
            &labels_,
            &field_threshold_,
            &field_lna_,
            &field_vga_,
            &button_mode_,
            &button_audio_,
            &text_status_,
            &text_freq_,
            &text_type_,
            &text_rssi_db_
        });

        // Standard-Empfangsverstärkung für maximale Reichweite mit Dual-Band-Antenne
        receiver_model.set_tuning_frequency(DRONE_CHANNELS[0].freq);
        receiver_model.set_lna(40); // Max LNA (40 dB)
        receiver_model.set_vga(32); // Solider VGA Gain
        receiver_model.set_rf_amp(true); // Interner Verstärker an
        receiver_model.enable();

        // Audio-Codec einschalten (für Kopfhörer / Lautsprecher am PortaPack H2)
        audio::output::start();

        // UI Initialwerte setzen
        field_threshold_.set_value(threshold_db_);
        field_lna_.set_value(receiver_model.lna());
        field_vga_.set_value(receiver_model.vga());

        // Event-Handler für Schwellenwert (Threshold)
        field_threshold_.on_change = [this](int32_t v) {
            threshold_db_ = v;
        };

        // Event-Handler für LNA & VGA Verstärkung
        field_lna_.on_change = [this](int32_t v) {
            receiver_model.set_lna(v);
        };
        field_vga_.on_change = [this](int32_t v) {
            receiver_model.set_vga(v);
        };

        // Umschalter zwischen "AUTO SCAN" (Wachmodus) und "PEIL-MODUS" (Richtung suchen)
        button_mode_.on_select = [this](Button&) {
            is_locked_df_mode_ = !is_locked_df_mode_;
            if (is_locked_df_mode_) {
                button_mode_.set_text("MODUS: PEILEN (LOCK)");
                // Auf die zuletzt stärkste erkannte Drohnen-Frequenz festsetzen
                current_channel_idx_ = last_alert_channel_idx_;
                tune_to_channel(current_channel_idx_);
            } else {
                button_mode_.set_text("MODUS: AUTO-SCAN");
            }
            set_dirty();
        };

        // Stummschalter für den akustischen Alarm (falls Lautlosigkeit im Feld nötig)
        button_audio_.on_select = [this](Button&) {
            audio_enabled_ = !audio_enabled_;
            button_audio_.set_text(audio_enabled_ ? "TON: EIN" : "TON: LAUTLOS");
            if (!audio_enabled_) {
                stop_beep();
            }
        };
    }

    ~DroneGuardView() {
        stop_beep();
        audio::output::stop();
        receiver_model.disable();
        baseband::shutdown();
    }

    void focus() override {
        field_threshold_.focus();
    }

    std::string title() const override { return "Drone Guard"; }

    // Zeichnet den Hintergrund (wird komplett ROT bei Drohnen-Alarm!) und den Peil-Balken
    void paint(Painter& painter) override {
        // Hintergrundfarbe je nach Status: Knallrot bei Alarm, Blau im Peilmodus, sonst Dunkelgrau
        Color bg_color = Color::black();
        if (alert_active_) {
            bg_color = Color::red();
        } else if (is_locked_df_mode_) {
            bg_color = Color::dark_blue();
        }

        // Kompletten Bildschirm einfärben
        painter.fill_rectangle(screen_rect(), bg_color);

        // Visuellen Peil-Balken (Direction Finding Bar) in der unteren Bildschirmhälfte zeichnen
        // Skaliert von -110 dBm (0%) bis -20 dBm (100%)
        int32_t clamped_db = std::clamp(current_rssi_db_, -110, -20);
        int32_t bar_width = ((clamped_db + 110) * 220) / 90;

        // Rahmen für Peil-Anzeige
        painter.draw_rectangle({10, 210, 220, 36}, Color::white());
        
        // Füllfarbe des Balkens (Gelb bei starkem Signal, Grün bei normalem Signal)
        Color bar_color = alert_active_ ? Color::yellow() : Color::green();
        if (bar_width > 0) {
            painter.fill_rectangle({12, 212, bar_width, 32}, bar_color);
        }

        // Schwellenwert-Markierung (Linie) im Balken einzeichnen
        int32_t thresh_clamped = std::clamp(threshold_db_, -110, -20);
        int32_t thresh_x = 10 + ((thresh_clamped + 110) * 220) / 90;
        painter.fill_rectangle({thresh_x, 206, 3, 44}, Color::white());
    }

private:
    NavigationView& nav_;

    size_t current_channel_idx_ { 0 };
    size_t last_alert_channel_idx_ { 0 };
    
    int32_t current_rssi_db_ { -100 };
    int32_t threshold_db_ { -65 }; // Schwellenwert in dBm (im Feld anpassbar)
    
    bool alert_active_ { false };
    bool is_locked_df_mode_ { false };
    bool audio_enabled_ { true };
    uint8_t hold_ticks_ { 0 };

    // UI-Beschriftungen
    Labels labels_ {
        { { 1 * 8, 1 * 16 }, "DROHNEN-WARNER & PEILER", Color::white() },
        { { 1 * 8, 3 * 16 }, "Schwelle:", Color::light_grey() },
        { { 16 * 8, 3 * 16 }, "dBm", Color::light_grey() },
        { { 1 * 8, 5 * 16 }, "LNA:", Color::light_grey() },
        { { 11 * 8, 5 * 16 }, "VGA:", Color::light_grey() },
        { { 1 * 8, 12 * 16 }, "RICHTUNGS-PEILUNG (RSSI):", Color::white() }
    };

    // Einstellbare Felder
    NumberField field_threshold_ {
        { 11 * 8, 3 * 16 }, 4, { -110, -10 }, 1, ' '
    };

    NumberField field_lna_ {
        { 6 * 8, 5 * 16 }, 2, { 0, 40 }, 8, ' '
    };

    NumberField field_vga_ {
        { 16 * 8, 5 * 16 }, 2, { 0, 62 }, 2, ' '
    };

    // Steuer-Buttons
    Button button_mode_ {
        { 1 * 8, 7 * 16, 16 * 8, 28 },
        "MODUS: AUTO-SCAN"
    };

    Button button_audio_ {
        { 18 * 8, 7 * 16, 11 * 8, 28 },
        "TON: EIN"
    };

    // Dynamische Status-Anzeigen
    Text text_status_ {
        { 1 * 8, 9 * 16 + 4, 28 * 8, 16 },
        "STATUS: BEREIT / SCANNE..."
    };

    Text text_freq_ {
        { 1 * 8, 10 * 16 + 6, 28 * 8, 16 },
        "Freq: ---"
    };

    Text text_type_ {
        { 1 * 8, 11 * 16 + 4, 28 * 8, 16 },
        "Typ:  ---"
    };

    Text text_rssi_db_ {
        { 1 * 8, 16 * 16, 28 * 8, 16 },
        "Signal: -100 dBm"
    };

    // Wechselt die Empfangsfrequenz des MAX2837 / RFFC5072 Tuners
    void tune_to_channel(size_t idx) {
        receiver_model.set_tuning_frequency(DRONE_CHANNELS[idx].freq);
    }

    // Erzeugt einen Alarm- oder Peilton über das Baseband
    void trigger_beep(uint32_t freq_hz) {
        if (!audio_enabled_) return;
        // Sendet Ton-Kommando an den M0 Baseband-Prozessor (Samplerate 24kHz)
        baseband::set_tone(0, freq_hz * 179, 24000);
    }

    void stop_beep() {
        baseband::set_tone(0, 0, 0);
    }

    // Wird vom System aufgerufen, sobald neue RSSI-Samples vom Baseband-Prozessor vorliegen (~60 Hz)
    void on_statistics_update(const ChannelStatistics& statistics) {
        // Umrechnung des Roh-RSSI in ungefähre dBm
        current_rssi_db_ = statistics.max_db;

        const auto& current_chan = DRONE_CHANNELS[current_channel_idx_];

        // Prüfen, ob das Signal den Schwellenwert überschreitet
        if (current_rssi_db_ >= threshold_db_) {
            bool state_changed = !alert_active_;
            alert_active_ = true;
            last_alert_channel_idx_ = current_channel_idx_;
            hold_ticks_ = 15; // Alarm für kurze Zeit halten, damit das Display nicht flackert

            text_status_.set("!!! DROHNEN-ALARM !!!");
            text_freq_.set(std::string("Kanal: ") + current_chan.label);
            text_type_.set(std::string("Typ:   ") + current_chan.type);

            // Akustischer Alarm / Peilton (Tonhöhe steigt mit der Signalstärke von 400 Hz bis 2800 Hz!)
            // Perfekt, um beim Drehen mit der Antenne ohne hinzusehen die Richtung zu hören!
            int32_t norm_strength = std::clamp(current_rssi_db_ - threshold_db_, 0L, 45L);
            uint32_t pitch_hz = 600 + (norm_strength * 45);
            trigger_beep(pitch_hz);

            if (state_changed) {
                set_dirty(); // Komplettes Display sofort ROT zeichnen
            }
        } else {
            if (hold_ticks_ > 0) {
                hold_ticks_--;
            } else {
                bool state_changed = alert_active_;
                alert_active_ = false;
                stop_beep();

                if (is_locked_df_mode_) {
                    text_status_.set("PEIL-MODUS (Drehe Antenne)");
                    text_freq_.set(std::string("Lock: ") + current_chan.label);
                    text_type_.set(std::string("Typ:  ") + current_chan.type);
                } else {
                    text_status_.set("SUCHE... (2.4 / 5.8 GHz)");
                    text_freq_.set(std::string("Scan: ") + current_chan.label);
                    text_type_.set("Alles ruhig.");
                }

                if (state_changed) {
                    set_dirty(); // Roten Bildschirm wieder zurücksetzen
                }
            }
        }

        // Numerische dBm-Anzeige und Richtungs-Balken aktualisieren
        text_rssi_db_.set("Signal: " + to_string_dec_int(current_rssi_db_) + " dBm (Max-Peilung)");

        // Wenn wir im Auto-Scan sind und gerade kein aktiver Alarm auf dem Kanal anliegt -> Nächste Frequenz!
        if (!is_locked_df_mode_ && hold_ticks_ == 0) {
            current_channel_idx_ = (current_channel_idx_ + 1) % DRONE_CHANNELS.size();
            tune_to_channel(current_channel_idx_);
        }

        // Peil-Balken neu zeichnen
        set_dirty();
    }

    // Message-Handler, der die Echtzeit-Statistiken vom M0-Prozessor empfängt
    MessageHandlerRegistration message_handler_stats_ {
        Message::ID::ChannelStatistics,
        [this](const Message* const p) {
            on_statistics_update(static_cast<const ChannelStatisticsMessage*>(p)->statistics);
        }
    };
};

} /* namespace ui */

#endif /* __UI_DRONE_GUARD_H__ */
