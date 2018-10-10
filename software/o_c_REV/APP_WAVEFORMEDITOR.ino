// Copyright (c) 2018, Jason Justian
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include "HSApplication.h"
#include "HSMIDI.h"
#include "vector_osc/HSVectorOscillator.h"
#include "vector_osc/WaveformManager.h"

class WaveformEditor : public HSApplication, public SystemExclusiveHandler {
public:
	void Start() {
	    if (!WaveformManager::Validate()) {
	        WaveformManager::Setup();
	    }
        waveform_number = 0;
	    Resume();
	}

	void Resume() {
        segment_number = 0;
        waveform_count = WaveformManager::WaveformCount();
        segments_remaining = WaveformManager::SegmentsRemaining();
        SwitchWaveform(waveform_number);
	}

    void Controller() {
        // Receive MIDI dumps
        ListenForSysEx();

        // LFO Outputs
        Out(0, test[0].Next());
        Out(1, test[1].Next());

        for (byte t = 2; t < 4; t++)
        {
            if (Gate(t)) {
                if (!gated[t]) { // Gate wasn't on last time, so start the waveform
                    test[t].Start();
                }
                gated[t] = 1;
            } else {
                if (gated[t]) { // Gate isn't on now, but was on last time, so release
                    test[t].Release();
                }
                gated[t] = 0;
            }
            Out(t, test[t].Next());
        }
    }

    void View() {
        gfxHeader("Waveform Editor");
        if (add_delete_confirm) DrawAddDelete();
        else DrawInterface();
    }

    void OnSendSysEx() { // Left Enc Push
        byte V[33];

        // There are 64 waveform segments, each containing two bytes. These will be
        // sent in four groups of 16 segments, for 32 bytes per segment. Each SysEx
        // payload will be 33 bytes (before packing), which includes the group
        // number.
        for (byte gr = 0; gr < 4; gr++)
        {
            int ix = 0;
            V[ix++] = gr; // Add the group number, for future decoding
            for (byte s = 0; s < 16; s++)
            {
                byte seg_ix = (gr * 4) + s; // Segment index
                V[ix++] = HS::user_waveforms[seg_ix].level;
                V[ix++] = HS::user_waveforms[seg_ix].time;
            }

            UnpackedData unpacked;
            unpacked.set_data(ix, V);
            PackedData packed = unpacked.pack();
            SendSysEx(packed, 'W');
        }
    }

    void OnReceiveSysEx() {
        uint8_t V[35];
        if (ExtractSysExData(V, 'W')) {
            int ix = 0;
            byte gr = V[ix++];
            for (byte s = 0; s < 16; s++)
            {
                byte seg_ix = (gr * 4) + s;
                HS::user_waveforms[seg_ix].level = V[ix++];
                HS::user_waveforms[seg_ix].time = V[ix++];
            }

            waveform_number = 0;
            Resume();
        }
    }

    /////////////////////////////////////////////////////////////////
    // Control handlers
    /////////////////////////////////////////////////////////////////
    void OnLeftButtonPress() {
        if (add_delete_confirm) {
            add_delete_confirm = 0;
        } else {
            AddSegment();
        }
    }

    void OnLeftButtonLongPress() {
        if (add_delete_confirm) {
            add_delete_confirm = 0;
        } else {
            DeleteSegment();
        }
    }

    void OnRightButtonPress() {
        if (add_delete_confirm) {
            AddOrDeleteWaveform();
            add_delete_confirm = 0;
        } else {
            cursor = 1 - cursor;
        }
    }

    void OnUpButtonPress() {
        byte old = waveform_number;
        waveform_number = constrain(waveform_number + 1, 0, waveform_count - 1);
        if (old != waveform_number) SwitchWaveform(waveform_number);
    }

    void OnDownButtonPress() {
        byte old = waveform_number;
        waveform_number = constrain(waveform_number - 1, 0, waveform_count - 1);
        if (old != waveform_number) SwitchWaveform(waveform_number);
    }

    void OnDownButtonLongPress() {
        add_delete_confirm = 1 - add_delete_confirm;
        add_waveform = 1; // Default to Add
    }

    void OnLeftEncoderMove(int direction) {
        if (add_delete_confirm) {
            add_waveform = 1 - add_waveform;
        } else {
            if (direction < 0 && segment_number > 0) --segment_number;
            if (direction > 0) segment_number = constrain(segment_number + 1, 0, osc.SegmentCount() - 1);
        }
    }

    void OnRightEncoderMove(int direction) {
        if (add_delete_confirm) {
            add_waveform = 1 - add_waveform;
        } else {
            VOSegment seg = osc.GetSegment(segment_number);
            if (cursor == 0) { // Level
                if (direction < 0 && seg.level > 0) seg.level = seg.level - 1;
                if (direction > 0 && seg.level < 255) seg.level = seg.level + 1;
            } else {
                if (direction < 0 && seg.time > 0) seg.time = seg.time - 1;
                if (direction > 0 && seg.time < 8) seg.time = seg.time + 1;
            }
            osc.SetSegment(segment_number, seg);
            if (osc.TotalTime() == 0) {
                // If the edit would reduce the total time to 0, force this segment's time to 1
                seg.time = 1;
                osc.SetSegment(segment_number, seg);
            }

            for (byte t = 0; t < 4; t++) test[t].SetSegment(segment_number, seg);
            WaveformManager::Update(waveform_number, segment_number, &seg);
        }
    }

private:
    bool cursor = 0; // 0 = Level, 1 = Time
    byte segment_number = 0;
    byte waveform_count;
    byte segments_remaining;

    bool add_delete_confirm = 0; // 1=Show add/delete confirmation screen
    bool add_waveform = 1; // 1=Add waveform, 0=Delete waveform

    // Info about currently-selected waveform
    int waveform_number = 0;
    VectorOscillator osc;

    // Test Waveforms
    VectorOscillator test[4];
    bool gated[4];

    void DrawInterface() {
        // Header
        gfxIcon(106, 0, SEGMENT_ICON);
        gfxPrint(116 + pad(10, segments_remaining), 1, segments_remaining);

        // Segment info
        VOSegment seg = osc.GetSegment(segment_number);
        gfxIcon(0, 15, WAVEFORM_ICON);
        gfxPrint(10 + pad(10, waveform_number + 1), 15, waveform_number + 1);
        gfxIcon(30, 15, SEGMENT_ICON);
        gfxPrint(40 + pad(10, segment_number + 1), 15, segment_number + 1);
        gfxIcon(64, 15, UP_DOWN_ICON);
        gfxPrint(74 + pad(100, seg.level - 128), 15, seg.level - 128);
        gfxIcon(112, 15, LEFT_RIGHT_ICON);
        gfxPrint(122, 15, seg.time);

        // Cursor
        if (cursor == 0) gfxCursor(74, 23, 24);
        else gfxCursor(122, 23, 6);

        DrawWaveform();
    }

    void DrawWaveform() {
        uint16_t total_time = osc.TotalTime();
        byte prev_x = 0; // Starting coordinates
        byte prev_y = 63;
        for (byte i = 0; i < osc.SegmentCount(); i++)
        {
            VOSegment seg = osc.GetSegment(i);
            byte y = 63 - Proportion(seg.level, 255, 40);
            byte seg_x = Proportion(seg.time, total_time, 128);
            byte x = prev_x + seg_x;
            byte p = segment_number == i ? 1 : 2;
            gfxDottedLine(prev_x, prev_y, x, y, p);
            prev_x = x;
            prev_y = y;
        }

        // Zero line
        gfxDottedLine(0, 43, 127, 43, 8);
    }

    void DrawAddDelete() {
        if (segments_remaining > 2) {
            gfxPrint(12, 15, "Add Waveform ");
            gfxPrint(waveform_count + 1);
        } else {
            // Not enough segments for a new waveform
            gfxPrint(12, 15, "(Cannot Add)");
        }

        if (waveform_count > 1) {
            gfxPrint(12, 25, "Del Waveform ");
            gfxPrint(waveform_number + 1);
        } else {
            // Can't delete the last waveform
            gfxPrint(12, 25, "(Cannot Del)");
        }

        if (add_waveform) gfxIcon(1, 15, CHECK_ICON);
        else gfxIcon(1, 25, CHECK_ICON);

        gfxPrint(0, 55, "[CANCEL]");
        gfxPrint(104, 55, "[OK]");
    }

    void SwitchWaveform(byte waveform_number_) {
        waveform_number = waveform_number_;
        osc = WaveformManager::VectorOscillatorFromWaveform(waveform_number);
        segment_number = 0;

        for (byte t = 0; t < 4; t++)
        {
            test[t] = WaveformManager::VectorOscillatorFromWaveform(waveform_number);
            test[t].SetScale((12 << 7) * 3);
            gated[t] = 0;
        }

        test[0].SetFrequency(1000); // Test 0: LFO 10Hz

        test[1].SetFrequency(44000); // Test 1: LFO Audio Rate

        test[2].SetFrequency(50); // Test 2: Bipolar Envelope
        test[2].Cycle(0);
        test[2].Sustain();

        test[3].SetFrequency(50); // Test 3: Unipolar Envelope
        test[3].Offset((12 << 7) * 3);
        test[3].Cycle(0);
        test[3].Sustain();
    }

    void AddSegment() {
        // If there are any segments left, and there are fewer than VO_MAX_SEGMENTS in this waveform, add a segment
        if (segments_remaining < HS::VO_SEGMENT_COUNT && osc.SegmentCount() < HS::VO_MAX_SEGMENTS) {
            WaveformManager::AddSegmentToWaveformAtSegmentIndex(waveform_number, segment_number);
            byte prev_segment_number = segment_number;
            SwitchWaveform(waveform_number);
            segment_number = prev_segment_number + 1;
            --segments_remaining;
        }
    }

    void DeleteSegment() {
        if (osc.SegmentCount() > 2) {
            bool ok_to_delete = 1;

            // The segment cannot be deleted if this would result in a 0 total time
            if (osc.GetSegment(segment_number).time == osc.TotalTime()) ok_to_delete = 0;

            if (ok_to_delete) {
                WaveformManager::DeleteSegmentFromWaveformAtSegmentIndex(waveform_number, segment_number);
                byte prev_segment_number = segment_number;
                SwitchWaveform(waveform_number);
                segment_number = prev_segment_number;
                if (segment_number > osc.SegmentCount() - 1) segment_number--;
            }
        }
    }

    void AddOrDeleteWaveform() {
        if (add_waveform) { // ADD
            if (segments_remaining > 2) {
                WaveformManager::AddWaveform();
                segments_remaining -= 3;
                waveform_number = waveform_count;
                waveform_count++;
                SwitchWaveform(waveform_number);
            }
        } else { // DELETE
            if (waveform_count > 1) {
                WaveformManager::DeleteWaveform(waveform_number);
                waveform_count--;
                if (waveform_number > waveform_count - 1) waveform_number = waveform_count - 1;
                segments_remaining = WaveformManager::SegmentsRemaining();
                SwitchWaveform(waveform_number);
            }
        }
    }
};

WaveformEditor WaveformEditor_instance;

// App stubs
void WaveformEditor_init() {
    WaveformEditor_instance.BaseStart();
}

// Not using O_C Storage
size_t WaveformEditor_storageSize() {return 0;}
size_t WaveformEditor_save(void *storage) {return 0;}
size_t WaveformEditor_restore(const void *storage) {return 0;}

void WaveformEditor_isr() {
	return WaveformEditor_instance.BaseController();
}

void WaveformEditor_handleAppEvent(OC::AppEvent event) {
    if (event ==  OC::APP_EVENT_RESUME) {
        WaveformEditor_instance.Resume();
    }
    if (event == OC::APP_EVENT_SUSPEND) {
        WaveformEditor_instance.OnSendSysEx();
    }
}

void WaveformEditor_loop() {} // Deprecated

void WaveformEditor_menu() {
    WaveformEditor_instance.BaseView();
}

void WaveformEditor_screensaver() {} // Deprecated

void WaveformEditor_handleButtonEvent(const UI::Event &event) {
    // For left encoder, handle press and long press
    if (event.control == OC::CONTROL_BUTTON_L) {
        if (event.type == UI::EVENT_BUTTON_LONG_PRESS) WaveformEditor_instance.OnLeftButtonLongPress();
        else WaveformEditor_instance.OnLeftButtonPress();
    }

    // For right encoder, only handle press (long press is reserved)
    if (event.control == OC::CONTROL_BUTTON_R && event.type == UI::EVENT_BUTTON_PRESS) WaveformEditor_instance.OnRightButtonPress();

    // For up button, handle only press (long press is reserved)
    if (event.control == OC::CONTROL_BUTTON_UP) WaveformEditor_instance.OnUpButtonPress();

    // For down button, handle press and long press
    if (event.control == OC::CONTROL_BUTTON_DOWN) {
        if (event.type == UI::EVENT_BUTTON_PRESS) WaveformEditor_instance.OnDownButtonPress();
        if (event.type == UI::EVENT_BUTTON_LONG_PRESS) WaveformEditor_instance.OnDownButtonLongPress();
    }
}

void WaveformEditor_handleEncoderEvent(const UI::Event &event) {
    // Left encoder turned
    if (event.control == OC::CONTROL_ENCODER_L) WaveformEditor_instance.OnLeftEncoderMove(event.value);

    // Right encoder turned
    if (event.control == OC::CONTROL_ENCODER_R) WaveformEditor_instance.OnRightEncoderMove(event.value);
}