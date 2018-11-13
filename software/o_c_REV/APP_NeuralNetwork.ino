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
#include "neuralnet/LogicGate.h"

// 9 sets of 24 bytes allocated for storage
#define NN_SETTING_LAST 216

class NeuralNetwork : public HSApplication, public SystemExclusiveHandler,
    public settings::SettingsBase<NeuralNetwork, NN_SETTING_LAST> {
public:
    void Start() {
        for (int ch = 0; ch < 4; ch++) output_neuron[ch] = ch;
    }
    
    void Resume() {
        LoadFromEEPROM();
    }

    void Controller() {
        ListenForSysEx();
        uint16_t tmp_state = source_state; // Set temporary state for this cycle
        // Check inputs
        for (byte i = 0; i < 8; i++)
        {
            bool set = (i < 4) ? Gate(i) : (In(i - 4) > HSAPPLICATION_3V);
            tmp_state = SetSource(tmp_state, i, set);
            input_state[i] = set; // For display
        }
        
        // Process neurons
        for (byte n = 0; n < 6; n++)
        {
            byte ix = (setup * 6) + n;
            bool set = neuron[ix].Calculate(tmp_state);
            tmp_state = SetSource(tmp_state, 8 + n, set);
        }
        
        // Set outputs based on assigned neuron's last state
        for (byte o = 0; o < 4; o++)
        {
            byte ix = (setup * 4) + o;
            byte n = output_neuron[ix] + (selected * 6);
            bool set = neuron[n].state;
            Out(o, set * HSAPPLICATION_5V);
        }

        source_state = tmp_state;
    }

    void View() {
        DrawInterface();
    }

    void OnSendSysEx() {
    }

    void OnReceiveSysEx() {
    }

    /////////////////////////////////////////////////////////////////
    // Control handlers
    /////////////////////////////////////////////////////////////////
    void OnLeftButtonPress() {
            screen = 1 - screen;
    }

    void OnLeftButtonLongPress() {
            all_connections = 1 - all_connections;
    }

    void OnRightButtonPress() {
            cursor++;
            byte ix = (setup * 6) + selected;
            if (cursor >= neuron[ix].NumParam()) cursor = 0;
            ResetCursor();
    }

    void OnUpButtonPress() {
            setup = constrain(setup + 1, 0, 3);
            LoadSetup(setup);
    }

    void OnDownButtonPress() {
            setup = constrain(setup - 1, 0, 3);
    }

    void OnDownButtonLongPress() {
            copy_mode = 1 - copy_mode;
    }

    void OnLeftEncoderMove(int direction) {
            selected = constrain(selected + direction, 0, 6);
            cursor = 0;
            ResetCursor();
    }

    void OnRightEncoderMove(int direction) {
            byte ix = (setup * 6) + selected;
            neuron[ix].UpdateValue(cursor, direction);
    }

private:
    // Screen and edit states
    byte cursor = 0; // Cursor on the neuron select screen
    int selected = 0; // 0-5, which neuron is currently selected
    int setup = 0; // 0-3, which set is currently selectd
    bool screen = 0; // 0 = selector screen, 1 = edit screen
    bool copy_mode = 0; // Copy mode on/off
    bool all_connections = 0; // Connections for all neurons shown instead of just selected
    
    LogicGate neuron[24]; // Four sets of six neurons
    byte output_neuron[16]; // Four sets of four output assignments
    bool input_state[8];
    
    // Source state is passed to each neuron for use in its logic gate calculation.
    // The value is a bitfield, with each bit indicating the value of a source's most-
    // recent result:
    //     Bits 0-3: Digital inputs
    //     Bits 4-7: CV inputs
    //     Bits 8-13: Neuron outputs
    uint16_t source_state = 0;

    void DrawInterface() {
        gfxHeader("Neural Network");
        if (screen) DrawEditScreen();
        else DrawSelectorScreen();
    }
    
    void DrawSelectorScreen() {
        // Draw the neurons
        byte offset_ix = setup * 6;
        for (byte n = 0; n < 6; n++)
        {
            byte ix = offset_ix + n;

            // Neurons are arranged in a 3x2 grid, top to bottom, left to right, like this:
            //     135
            //     246
            byte cell_x = n / 2;
            byte cell_y = n % 2;
            byte x = (cell_x * 32) + 24;
            byte y = (cell_y * 24) + 16;
            neuron[ix].DrawSmallAt(x, y);
            
            if (selected == ix && CursorBlink()) gfxLine(x, y, x, y + 24);
        }

        // Draw the inputs
        gfxPrint(0, 16, "DV");
        for (byte i = 0; i < 8; i++)
        {
            // Inputs are arranged in a 2x4 grid, top to bottom, left to right, like this:
            //     15
            //     26
            //     37
            //     48
            byte cell_x = i / 4;
            byte cell_y = i % 4;
            byte x = (cell_x * 6);
            byte y = (cell_y * 10) + 24;
            gfxPrint(x, y, cell_y + 1);
            
            if (input_state[i]) gfxInvert(x - 1, y - 1, 8, 9);
        }
        
        // Draw outputs
        byte x = 120;
        for (byte o = 0; o < 4; o++)
        {
            // Outputs are arranged top to bottom
            byte y = (o * 12) + 16;
            char out_name[2] = {static_cast<char>(o + 'A'), '\0'};
            gfxPrint(x, y, out_name);
            
            if (ViewOut(o)) gfxInvert(x - 1, y - 1, 8, 9);
        }
        if (selected == 6 && CursorBlink()) gfxFrame(116, 16, 116, 48);
    }
    
    void DrawEditScreen() {
        if (selected < 6) {
            // Draw the editor for a Neuron
            byte ix = (setup * 6) + selected;
            byte p = neuron[ix].NumParam(); // Number of parameters for this neuron
            for (byte c = 0; c < p; c++)
            {
                byte y = (c * 10) + 16;
                neuron[ix].PrintParamNameAt(64, y, c);
                neuron[ix].PrintValueAt(96, y, c);
                if (c == cursor) gfxCursor(96, y + 8, 31);
            }
        } else {
            // Draw the Output Assign editor
        }
    }
    
    void LoadSetup(byte setup_) {
            cursor = 0;
            selected = 0;
            setup = setup_;
    }
    
    uint16_t SetSource(uint16_t state, byte b, bool v)
    {
        if (v) state |= (0x01 << b);
        else state &= ~(0x01 << b);
        return state;
    }
    
    /* The system settings are just bytes. Move them into the instance variables here */
    void LoadFromEEPROM() {
            
    }
    
};

// Declare 216 bytes for storage
#define NN_EEPROM_DATA {0,0,255,"St",NULL,settings::STORAGE_TYPE_U8},
#define NN_DO_TWENTYFOUR_TIMES(A) A A A A A A A A A A A A A A A A A A A A A A A A
SETTINGS_DECLARE(NeuralNetwork, NN_SETTING_LAST) {
    NN_DO_TWENTYFOUR_TIMES(NN_EEPROM_DATA)
    NN_DO_TWENTYFOUR_TIMES(NN_EEPROM_DATA)
    NN_DO_TWENTYFOUR_TIMES(NN_EEPROM_DATA)
    NN_DO_TWENTYFOUR_TIMES(NN_EEPROM_DATA)
    NN_DO_TWENTYFOUR_TIMES(NN_EEPROM_DATA)
    NN_DO_TWENTYFOUR_TIMES(NN_EEPROM_DATA)
    NN_DO_TWENTYFOUR_TIMES(NN_EEPROM_DATA)
    NN_DO_TWENTYFOUR_TIMES(NN_EEPROM_DATA)
    NN_DO_TWENTYFOUR_TIMES(NN_EEPROM_DATA)
};

NeuralNetwork NeuralNetwork_instance;

// App stubs
void NeuralNetwork_init() {
    NeuralNetwork_instance.BaseStart();
}

size_t NeuralNetwork_storageSize() {return NeuralNetwork::storageSize();}
size_t NeuralNetwork_save(void *storage) {return NeuralNetwork_instance.Save(storage);}
size_t NeuralNetwork_restore(const void *storage) {return NeuralNetwork_instance.Restore(storage);}

void NeuralNetwork_isr() {
    return NeuralNetwork_instance.BaseController();
}

void NeuralNetwork_handleAppEvent(OC::AppEvent event) {
    if (event ==  OC::APP_EVENT_RESUME) {
        NeuralNetwork_instance.Resume();
    }
    if (event == OC::APP_EVENT_SUSPEND) {
        NeuralNetwork_instance.OnSendSysEx();
    }
}

void NeuralNetwork_loop() {} // Deprecated

void NeuralNetwork_menu() {
    NeuralNetwork_instance.BaseView();
}

void NeuralNetwork_screensaver() {} // Deprecated

void NeuralNetwork_handleButtonEvent(const UI::Event &event) {
    // For left encoder, handle press and long press
    if (event.control == OC::CONTROL_BUTTON_L) {
        if (event.type == UI::EVENT_BUTTON_LONG_PRESS) NeuralNetwork_instance.OnLeftButtonLongPress();
        else NeuralNetwork_instance.OnLeftButtonPress();
    }

    // For right encoder, only handle press (long press is reserved)
    if (event.control == OC::CONTROL_BUTTON_R && event.type == UI::EVENT_BUTTON_PRESS) NeuralNetwork_instance.OnRightButtonPress();

    // For up button, handle only press (long press is reserved)
    if (event.control == OC::CONTROL_BUTTON_UP) NeuralNetwork_instance.OnUpButtonPress();

    // For down button, handle press and long press
    if (event.control == OC::CONTROL_BUTTON_DOWN) {
        if (event.type == UI::EVENT_BUTTON_PRESS) NeuralNetwork_instance.OnDownButtonPress();
        if (event.type == UI::EVENT_BUTTON_LONG_PRESS) NeuralNetwork_instance.OnDownButtonLongPress();
    }
}

void NeuralNetwork_handleEncoderEvent(const UI::Event &event) {
    // Left encoder turned
    if (event.control == OC::CONTROL_ENCODER_L) NeuralNetwork_instance.OnLeftEncoderMove(event.value);

    // Right encoder turned
    if (event.control == OC::CONTROL_ENCODER_R) NeuralNetwork_instance.OnRightEncoderMove(event.value);
}