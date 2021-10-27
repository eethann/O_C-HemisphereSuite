// Copyright (c) 2021, Ethan Winn <ethan@destratify.com>
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

#include "braids_quantizer.h"
#include "braids_quantizer_scales.h"
#include "OC_scales.h"

#define INTERVALIC_1V (12 << 7)
#define INTERVALIC_1_12TH_V (INTERVALIC_1V / 12)

enum {
    INTERVALIC_SETTING_INTERVAL,
    INTERVALIC_SETTING_OFFSET_OR_SCALE,
    INTERVALIC_SETTING_ENABLED
};

enum {
    INTERVALIC_ADDER, 
    INTERVALIC_FREE,
    INTERVALIC_1_12th,
    INTERVALIC_2_12th,
    INTERVALIC_3_12th,
    INTERVALIC_4_12th,
    INTERVALIC_5_12th,
    INTERVALIC_6_12th,
    INTERVALIC_7_12th,
    INTERVALIC_8_12th,
    INTERVALIC_9_12th,
    INTERVALIC_10_12th,
    INTERVALIC_11_12th,
    INTERVALIC_x2,
    INTERVALIC_x3,
    INTERVALIC_x5,
    INTERVALIC_x7,
    INTERVALIC_x3_2,
    INTERVALIC_x5_4,
    INTERVALIC_x7_4
};

const char* const interval_names[20] = {
    "ADDER", 
    "FREE",
    "+1",
    "+2",
    "+3",
    "+4",
    "+5",
    "+6",
    "+7",
    "+8",
    "+9",
    "+10",
    "+11",
    "x2",
    "x3",
    "x5",
    "x7",
    "x3/2",
    "x5/4",
    "x7/4"
};

const float pow2_14 = pow(2,14);                
const simfloat simfloat_12th = (int32_t)(INTERVALIC_1_12TH_V * pow2_14);
const simfloat simfloat_log2_3 = (int32_t)(log2(3) * pow2_14);
const simfloat simfloat_log2_5 = (int32_t)(log2(5) * pow2_14);
const simfloat simfloat_log2_7 = (int32_t)(log2(7) * pow2_14);

// TODO see if this can optimized away from floats
// (it's not clear how to use something like simfloat w/ logs)
const simfloat intervals[20] = { 
    0,
    0,
    1 * simfloat_12th,
    // TODO determine if we need all these, might need to only have some to have
    // enough space for storage.
    2 * simfloat_12th,
    3 * simfloat_12th,
    4 * simfloat_12th,
    5 * simfloat_12th,
    6 * simfloat_12th,
    7 * simfloat_12th,
    8 * simfloat_12th,
    9 * simfloat_12th,
    10 * simfloat_12th,
    11 * simfloat_12th,
    12 * simfloat_12th,
    simfloat_log2_3 * INTERVALIC_1V,
    simfloat_log2_5 * INTERVALIC_1V,
    simfloat_log2_7 * INTERVALIC_1V,
    (simfloat_log2_3 - int2simfloat(1)) * INTERVALIC_1V,
    (simfloat_log2_5 - int2simfloat(2)) * INTERVALIC_1V,
    (simfloat_log2_7 - int2simfloat(2)) * INTERVALIC_1V
};

class Intervalic : public HemisphereApplet {
public:

    const char* applet_name() { // Maximum 10 characters
        return "Intervalic";
    }

	/* Run when the Applet is selected */
    void Start() {
        cursor = 0;
        ForEachChannel(ch) {
            quantizer[ch].Init();
            scale[ch] = 4;
            quantizer[ch].Configure(OC::Scales::GetScale(scale[ch]), 0xffff);
            offset[ch] = 1;
            if (ch == 0) {
                interval[ch] = INTERVALIC_ADDER;
                // TODO implement sample and hold instead of enabled for adders
                enabled[ch] = true;
            } else {
                interval[ch] = INTERVALIC_1_12th;
                enabled[ch] = false;
            }
        }
    }

	/* Run during the interrupt service routine, 16667 times per second */
    void Controller() {
        // TODO debug why interval count is always too high
        // TODO add support for 4 channel mode
        // TODO figure out simfloat vs float
        // TODO add start end end adc lag support to coordinate gate and cv timings
        int32_t interval_sum = 0;
        // First, go through all the non-adder channels and calculate the
        // sum across all.
        ForEachChannel(ch) {
            if (interval[ch] != INTERVALIC_ADDER) {
                // If the toggled state is enabled (enabled xor gate)
                // TODO check for ADC lag issue here
                if (enabled[ch] == (bool)Gate(ch)) {
                    Out(ch, 0);
                } else {
                    // Read CV for this channel
                    int32_t interval_cv = In(ch);
                    if (interval[ch] == INTERVALIC_FREE) {
                        // If this is a free interval, just pass the CV through
                        // TODO determine if we need to handle negative values
                        // Add to interval_sum
                        interval_sum += interval_cv;
                        // Output this interval
                        Out(ch, constrain(interval_cv, -HEMISPHERE_3V_CV, HEMISPHERE_MAX_CV));
                    } else {
                        // Otherwise use the CV to determine the number of intervals
                        // TODO determine if we need to handle negative values
                        int32_t num_intervals = offset[ch] + (interval_cv / INTERVALIC_1V);
                        // Multiply by interval amount
                        int32_t interval_val = simfloat2int(intervals[interval[ch]] * num_intervals);
                        // Output this interval
                        Out(ch, min(max(interval_val, -HEMISPHERE_3V_CV), HEMISPHERE_MAX_CV));
                        // Proportion(intervals[interval[ch]], INTERVALIC_1V, num_intervals);
                        // Add to interval_sum
                        interval_sum += interval_val;
                    }
                }
            }
        }
        ForEachChannel(ch) {
            if (interval[ch] == INTERVALIC_ADDER) {
                int32_t adder_cv = In(ch);
                adder_cv += (enabled[ch] != (bool)Gate(ch)) ? interval_sum : 0;
                adder_cv = quantizer[ch].Process(adder_cv, 0, 0);
                Out(ch, constrain(adder_cv,-HEMISPHERE_3V_CV, HEMISPHERE_MAX_CV));
            }
        }
    }

	/* Draw the screen */
    void View() {
        gfxHeader(applet_name());
        DrawInterface();
        // Add other view code as private methods
    }

	/* Called when the encoder button for this hemisphere is pressed */
    void OnButtonPress() {
        cursor = (cursor + 1) % 6;
        ResetCursor();
    }

	/* Called when the encoder for this hemisphere is rotated
	 * direction 1 is clockwise
	 * direction -1 is counterclockwise
	 */
    void OnEncoderMove(int direction) {
        int cursor_setting = cursor % 3;
        int cursor_ch = cursor / 3;
        switch (cursor_setting) {
            case INTERVALIC_SETTING_INTERVAL:
            interval[cursor_ch] = (interval[cursor_ch] + direction + 20) % 20;
            break;
            case INTERVALIC_SETTING_OFFSET_OR_SCALE:
            if (interval[cursor_ch] == INTERVALIC_ADDER) {
                scale[cursor_ch] += direction;
                if (scale[cursor_ch] >= OC::Scales::NUM_SCALES) scale[cursor_ch] = 0;
                if (scale[cursor_ch] < 0) scale[cursor_ch] = OC::Scales::NUM_SCALES - 1;
                quantizer[cursor_ch].Configure(OC::Scales::GetScale(scale[cursor_ch]), 0xffff);
            } else {
                offset[cursor_ch] = max(min(offset[cursor_ch] + direction, 12), -12);
            }
            break;
            case INTERVALIC_SETTING_ENABLED:
            enabled[cursor_ch] = !enabled[cursor_ch];
            break;
        }
    }
        
    /* Each applet may save up to 32 bits of data. When data is requested from
     * the manager, OnDataRequest() packs it up (see HemisphereApplet::Pack()) and
     * returns it.
     */
    // FIXME implement data loading and saving.
    uint32_t OnDataRequest() {
        uint32_t data = 0;
        // example: pack property_name at bit 0, with size of 8 bits
        // Pack(data, PackLocation {0,8}, property_name); 
        return data;
    }

    /* When the applet is restored (from power-down state, etc.), the manager may
     * send data to the applet via OnDataReceive(). The applet should take the data
     * and unpack it (see HemisphereApplet::Unpack()) into zero or more of the applet's
     * properties.
     */
    void OnDataReceive(uint32_t data) {
        // example: unpack value at bit 0 with size of 8 bits to property_name
        // property_name = Unpack(data, PackLocation {0,8}); 
    }

protected:
    /* Set help text. Each help section can have up to 18 characters. Be concise! */
    void SetHelp() {
        //                               "------------------" <-- Size Guide
        help[HEMISPHERE_HELP_DIGITALS] = "Toggle on/off";
        help[HEMISPHERE_HELP_CVS]      = "Root / +- Steps";
        help[HEMISPHERE_HELP_OUTS]     = "Sum / Interval Val";
        help[HEMISPHERE_HELP_ENCODER]  = "Type/Scale/Intrvl";
        //                               "------------------" <-- Size Guide
    }
    
private:
    int cursor;
    int interval[2];
    int16_t offset[2];
    bool enabled[2];
    // Quantizer for adder channels
    braids::Quantizer quantizer[2];
    int scale[2]; // Scale per channel

    void DrawInterface() {
        ForEachChannel(ch)
        {
            gfxPrint((1 + 31 * ch), 15, interval_names[interval[ch]]);
            if (interval[ch] == INTERVALIC_ADDER) {
                gfxPrint((1+ 31 * ch), 25, OC::scale_names_short[scale[ch]]);
            } else {
                gfxPrint((1+ 31 * ch), 25, offset[ch]);
            }
            gfxIcon((1+ 31 * ch), 35, enabled[ch] ? CHECK_ON_ICON : CHECK_OFF_ICON);
        }
        // Draw cursor
        int cursor_ch      = cursor / 3;
        int cursor_setting = cursor % 3;
        gfxCursor(31 * cursor_ch, 25 + 10 * cursor_setting, 12);
    }
};


////////////////////////////////////////////////////////////////////////////////
//// Hemisphere Applet Functions
///
///  Once you run the find-and-replace to make these refer to Intervalic,
///  it's usually not necessary to do anything with these functions. You
///  should prefer to handle things in the HemisphereApplet child class
///  above.
////////////////////////////////////////////////////////////////////////////////
Intervalic Intervalic_instance[2];

void Intervalic_Start(bool hemisphere) {Intervalic_instance[hemisphere].BaseStart(hemisphere);}
void Intervalic_Controller(bool hemisphere, bool forwarding) {Intervalic_instance[hemisphere].BaseController(forwarding);}
void Intervalic_View(bool hemisphere) {Intervalic_instance[hemisphere].BaseView();}
void Intervalic_OnButtonPress(bool hemisphere) {Intervalic_instance[hemisphere].OnButtonPress();}
void Intervalic_OnEncoderMove(bool hemisphere, int direction) {Intervalic_instance[hemisphere].OnEncoderMove(direction);}
void Intervalic_ToggleHelpScreen(bool hemisphere) {Intervalic_instance[hemisphere].HelpScreen();}
uint32_t Intervalic_OnDataRequest(bool hemisphere) {return Intervalic_instance[hemisphere].OnDataRequest();}
void Intervalic_OnDataReceive(bool hemisphere, uint32_t data) {Intervalic_instance[hemisphere].OnDataReceive(data);}
