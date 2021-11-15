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

#define INTERVALIC_UI_TOP 16
#define INTERVALIC_UI_LINE_HEIGHT 11
#define INTERVALIC_UI_COL_WIDTH 31
#define INTERVALIC_UI_SH_SHOW_TIME 50

enum {
    INTERVALIC_SETTING_INTERVAL,
    INTERVALIC_SETTING_BASE_OR_SCALE,
    INTERVALIC_SETTING_ENABLED
};

enum {
    INTERVALIC_ADDER, 
    INTERVALIC_OFFSET,
    INTERVALIC_SCALE_DEGREE,
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

const char* const interval_names[21] = {
    "ADDER", 
    "CV",
    "DEGR",
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
const simfloat intervals[21] = { 
    0,
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
    static int32_t interval_sum;
    static int32_t scale_degree_sum;
    static uint32_t last_tick;
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
            base[ch] = 1;
            continuous[ch] = true;
            adc_offset_clock[ch] = 0;
            adc_offset_gate[ch] = 0;
            toggle[ch] = false;
            if (ch == 0) {
                interval[ch] = INTERVALIC_1_12th;
            } else {
                interval[ch] = INTERVALIC_ADDER;
            }
        }
        interval_sum = 0;
        scale_degree_sum = 0;
        last_tick = OC::CORE::ticks;
    }

	/* Run during the interrupt service routine, 16667 times per second */
    void Controller() {
        // TODO add support for 4 channel mode
        // WIP: use statics for 4 channel support. This means that only
        // intervals to the left of adders will be included.
        // static int32_t interval_sum;
        // static int8_t last_ch;
        if (last_tick != OC::CORE::ticks) {
            interval_sum = 0;
            scale_degree_sum = 0;
            last_tick = OC::CORE::ticks;
        }

        // Loop over all channels, accumulate the interval as we go.
        // Note that this means that adder channels will only include intervals
        // to the left of them in the panel. This is done to enable 4 channel
        // operation, though that's still in progress.
        ForEachChannel(ch) {
            if (toggle[ch]) {
                // Since we defer updating adc_offset_gate, don't start ADC if
                // we are already waiting.
                // Note that this might start to glitch if clock have a period of
                // less than 33 cycles.
                if (NoActiveADCLag(ch) && Gate(ch) != adc_offset_gate[ch]) {
                    StartADCLag(ch);
                }
                if (EndOfADCLag(ch)) {
                    adc_offset_gate[ch] = Gate(ch);
                }
                // This was a test to see if it worked to use continuous with toggle
                // if (adc_offset_gate[ch]) {
                //     continuous[ch] = false;
                // }
                // update_channel_cv[ch] = continuous[ch] || adc_offset_gate[ch];
                // bypass_channel_cv[ch] = !continuous[ch] && !adc_offset_gate[ch];
                update_channel_cv[ch] = adc_offset_gate[ch];
                update_channel_cv_ui[ch] = update_channel_cv[ch] ? 1 : 0;
                bypass_channel_cv[ch] = !adc_offset_gate[ch];
            } else {
                // if (Clock(ch) != adc_offset_clock[ch]) {
                //     adc_offset_clock[ch] = Clock(ch);
                //     if (adc_offset_clock[ch]) {
                //     }
                // }
                if (Clock(ch) && NoActiveADCLag(ch)) {
                    StartADCLag(ch);
                    continuous[ch] = false;
                }
                update_channel_cv[ch] = (continuous[ch] || EndOfADCLag(ch));
                // Set the update channel ui flag to update on the next ui draw
                if (!update_channel_cv_ui[ch] && update_channel_cv[ch]) {
                    update_channel_cv_ui[ch] = INTERVALIC_UI_SH_SHOW_TIME;
                }
                bypass_channel_cv[ch] = false;
            }
            if (interval[ch] == INTERVALIC_ADDER) {
                // If continuous, toggling (low or high), or in s/h and clocked,
                // update adder CV. This is different from intervals, that
                // output zero in toggle with low gate.
                if (update_channel_cv[ch] || bypass_channel_cv[ch]) {
                    channel_cv[ch] = In(ch);
                    if (!bypass_channel_cv[ch]) {
                        channel_cv[ch] += interval_sum;
                    } 
                    channel_cv[ch] = quantizer[ch].Process(channel_cv[ch], 0, scale_degree_sum);
                }
            } else if (interval[ch] == INTERVALIC_SCALE_DEGREE) {
                if (update_channel_cv[ch]) {
                    channel_cv[ch] = In(ch);
                    // TODO look into using the interval approach for this
                    // (e.g. specify an interval as scale degree, then a base amount to use)
                    scale_degree_sum += channel_cv[ch] / 128 + base[ch];
                } else if (bypass_channel_cv[ch]) {
                    channel_cv[ch] = 0;
                }
            } else {
                // Handle interval channel
                if (update_channel_cv[ch]) {
                    // Read CV for this channel
                    channel_cv[ch] = In(ch);
                    if (interval[ch] == INTERVALIC_OFFSET) {
                        channel_cv[ch] += base[ch] * 128;
                    } else {
                        // Otherwise use the CV to determine the number of intervals
                        // TODO determine if we need to handle negative values
                        int32_t num_intervals = base[ch] + (channel_cv[ch] / INTERVALIC_1V);
                        // Multiply by interval amount
                        channel_cv[ch] = simfloat2int(intervals[interval[ch]] * num_intervals);
                    } 
                } else if (bypass_channel_cv[ch]) {
                    channel_cv[ch] = 0;
                }
                interval_sum += channel_cv[ch];
            }
            Out(ch, constrain(channel_cv[ch], -HEMISPHERE_3V_CV, HEMISPHERE_MAX_CV));
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
            interval[cursor_ch] = (interval[cursor_ch] + direction + 21) % 21;
            if (interval[cursor_ch] == INTERVALIC_ADDER) {
                // Set new adder to continuous at start.
                continuous[cursor_ch] = 1;
            }
            break;
            case INTERVALIC_SETTING_BASE_OR_SCALE:
            if (interval[cursor_ch] == INTERVALIC_ADDER) {
                // Reset continuous on scale change.
                continuous[cursor_ch] = 1;
                scale[cursor_ch] += direction;
                if (scale[cursor_ch] >= OC::Scales::NUM_SCALES) scale[cursor_ch] = 0;
                if (scale[cursor_ch] < 0) scale[cursor_ch] = OC::Scales::NUM_SCALES - 1;
                quantizer[cursor_ch].Configure(OC::Scales::GetScale(scale[cursor_ch]), 0xffff);
            } else {
                base[cursor_ch] = max(min(base[cursor_ch] + direction, 12), -12);
            }
            break;
            case INTERVALIC_SETTING_ENABLED:
            toggle[cursor_ch] = !toggle[cursor_ch];
            // Reset continuous operation when s/h / toggle mode is toggled
            continuous[cursor_ch] = 1;
            break;
        }
    }
        
    /* Each applet may save up to 32 bits of data. When data is requested from
     * the manager, OnDataRequest() packs it up (see HemisphereApplet::Pack()) and
     * returns it.
     */
    uint32_t OnDataRequest() {
        uint32_t data = 0;
        Pack(data, PackLocation {0,5}, interval[0]);
        Pack(data, PackLocation {5,5}, interval[1]);
        Pack(data, PackLocation {10,1}, toggle[0]);
        Pack(data, PackLocation {11,1}, toggle[1]);
        int delta = 0;
        ForEachChannel(ch) {
            if (interval[ch] == INTERVALIC_ADDER) {
                Pack(data, PackLocation{12 + delta, 8}, (int8_t)scale[ch]);
                delta += 8;
            } else {
                Pack(data, PackLocation{12 + delta, 4}, base[ch]);
                delta += 4;
            } 
        }
        return data;
    }

    /* When the applet is restored (from power-down state, etc.), the manager may
     * send data to the applet via OnDataReceive(). The applet should take the data
     * and unpack it (see HemisphereApplet::Unpack()) into zero or more of the applet's
     * properties.
     */
    void OnDataReceive(uint32_t data) {
        interval[0] = Unpack(data, PackLocation {0,5});
        interval[1] = Unpack(data, PackLocation {5,5});
        toggle[0] = Unpack(data, PackLocation {10,1});
        toggle[1] = Unpack(data, PackLocation {11,1});
        int delta = 0;
        ForEachChannel(ch) {
            if (interval[ch] == INTERVALIC_ADDER) {
                scale[ch] = Unpack(data, PackLocation{12 + delta, 8});
                base[ch] = 1;
                delta += 8;
            } else {
                base[ch] = Unpack(data, PackLocation{12 + delta, 4});
                scale[ch] = 4;
                delta += 4;
            } 
            quantizer[ch].Configure(OC::Scales::GetScale(scale[ch]), 0xffff);
        }
    }

protected:
    /* Set help text. Each help section can have up to 18 characters. Be concise! */
    void SetHelp() {
        //                               "------------------" <-- Size Guide
        help[HEMISPHERE_HELP_DIGITALS] = "Toggle";
        help[HEMISPHERE_HELP_CVS]      = "Val";
        help[HEMISPHERE_HELP_OUTS]     = "Sum / Interval";
        help[HEMISPHERE_HELP_ENCODER]  = "Type/Scale/Intrvl";
        //                               "------------------" <-- Size Guide
    }
    
private:
    int cursor;
    bool continuous[2]; // Each channel starts as continuous and becomes clocked when a clock is received
    int32_t channel_cv[2]; // Store each interval cv for sample and hold
    uint update_channel_cv_ui[2];
    bool update_channel_cv[2];
    bool bypass_channel_cv[2];

    // Cache gate to detect change
    bool adc_offset_gate[2];
    bool adc_offset_clock[2];

    // Quantizer for adder channels
    braids::Quantizer quantizer[2];

    // Settings
    int interval[2]; // 5 bits each = 10
    // base is the start # of intervals to offset by in interval mode
    // in offset mode it's the # of semis to quantize to
    // or the number of steps by default
    int16_t base[2]; // could be 3 bits each = 6

    // TODO refactor toggle to toggle between s/h and enabled
    bool toggle[2]; // 1 bit each = 2
    int scale[2]; // Scale per channel, 7 bits each?
    // Whether this channel should output an accumulation of all prior or just
    // itself. For adders whether it should contribute to the interval sum.
    bool accumulator[2]; 

    int uiColX(int col) {
        return (1 + INTERVALIC_UI_COL_WIDTH * col);
    }

    int uiLineY(int line_num) {
        return INTERVALIC_UI_TOP + INTERVALIC_UI_LINE_HEIGHT * line_num;
    }

    void DrawInterface() {
        int ch_col_x;
        ForEachChannel(ch) {
            ch_col_x = uiColX(ch);
            switch (interval[ch]) {
                case INTERVALIC_ADDER:
                gfxPrint(ch_col_x, uiLineY(0), "ADDER");
                gfxPrint(ch_col_x, uiLineY(1), "scal:");
                gfxPrint(ch_col_x, uiLineY(2), OC::scale_names_short[scale[ch]]);
                break;
                case INTERVALIC_OFFSET:
                gfxPrint(ch_col_x, uiLineY(0), "OFFST");
                gfxPrint(ch_col_x, uiLineY(1), "qntz:");
                if (base[ch]) {
                    gfxPrint(ch_col_x, uiLineY(2), base[ch]);
                    gfxPrint(ch_col_x + 12, uiLineY(2), "S");
                } else {
                    gfxPrint(ch_col_x, uiLineY(2), "off");
                }
                break;
                case INTERVALIC_SCALE_DEGREE:
                gfxPrint(ch_col_x, uiLineY(0), "TRNSP");
                gfxPrint(ch_col_x, uiLineY(1), "step:");
                gfxPrint(ch_col_x, uiLineY(2), base[ch]);
                break;
                default:
                gfxPrint(ch_col_x, uiLineY(0), "INTVL");
                gfxPrint(ch_col_x, uiLineY(1), interval_names[interval[ch]]);
                gfxPrint(ch_col_x, uiLineY(2), "#:");
                gfxPrint(ch_col_x + 12, uiLineY(2), base[ch]);
                break;
            }
            if (toggle[ch]) {
                gfxPrint(ch_col_x, uiLineY(3), "TOGL");
                if (update_channel_cv_ui[ch]) {
                    gfxIcon(ch_col_x + 24, uiLineY(3), CHECK_ON_ICON);
                } else {
                    gfxIcon(ch_col_x + 24, uiLineY(3), CHECK_OFF_ICON);
                }
            } else {
                gfxPrint(ch_col_x, uiLineY(3), "S/H");
                if (continuous[ch]) {
                    gfxIcon(ch_col_x + 24, uiLineY(3), PLAY_ICON);
                } else {
                    if (update_channel_cv_ui[ch]) {
                        gfxIcon(ch_col_x + 24, uiLineY(3), RECORD_ICON);
                        update_channel_cv_ui[ch]--;
                    } else {
                        gfxIcon(ch_col_x + 24, uiLineY(3), PLAY_ICON);
                    }
                }
            }
            // if (update_channel_cv[ch]) {
            //     gfxPrint((ch_col_x), 42, "u");
            // }
            // if (bypass_channel_cv[ch]) {
            //     gfxPrint((9 + 31 * ch), 42, "b");
            // }
            // if (continuous[ch]) {
            //     gfxPrint((17 + 31 * ch), 42, "c");
            // }
        }
        // Draw cursor
        int cursor_ch      = cursor / 3;
        int cursor_setting = cursor % 3;
        if (cursor_setting > 0 || interval[cursor_ch] > INTERVALIC_SCALE_DEGREE) {
            gfxCursor(uiColX(cursor_ch), uiLineY(cursor_setting + 2) - 3, 12);
        } else {
            gfxCursor(uiColX(cursor_ch), uiLineY(cursor_setting + 1) - 3, 12);
        }
        if (cursor_setting == 0){
            if (interval[cursor_ch] > INTERVALIC_SCALE_DEGREE) {
                gfxInvert(uiColX(cursor_ch), uiLineY(0) - 2, 31, INTERVALIC_UI_LINE_HEIGHT * 2);
            } else {
                gfxInvert(uiColX(cursor_ch), uiLineY(0) - 2, 31, INTERVALIC_UI_LINE_HEIGHT);
            }
        } else {
            gfxInvert(uiColX(cursor_ch), uiLineY(cursor_setting + 1) - 2, 31, INTERVALIC_UI_LINE_HEIGHT);
        }
        gfxFrame(uiColX(cursor_ch), uiLineY(0) - 2, 31, INTERVALIC_UI_LINE_HEIGHT * 4);
    }
};

// Initialize the interval static to track between hemispheres
int32_t Intervalic::interval_sum = 0;
int32_t Intervalic::scale_degree_sum = 0;
uint32_t Intervalic::last_tick = 0;

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
