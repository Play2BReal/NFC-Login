#include "nfc_login_hid.h"
#include "../settings/nfc_login_settings.h"

#include <furi_hal_usb.h>
#include <furi_hal_usb_hid.h>
#include <furi_hal_bt.h>
#include <furi_hal_version.h>
#include <usb_hid.h>
#include <furi.h>

// BLE HID profile - try to use even though some symbols are disabled
// We can use bt_profile_start (enabled) to start the profile
// Undefine first in case header already defined it
#undef HAS_BLE_HID_API

// Try to include BLE HID headers - we'll use bt_profile_start which is enabled
#ifdef __has_include
    #if __has_include(<extra_profiles/hid_profile.h>) && __has_include(<bt/bt_service/bt.h>)
        #include <extra_profiles/hid_profile.h>
        #include <bt/bt_service/bt.h>
        // Enable BLE HID - we'll use bt_profile_start and try to access functions
        #define HAS_BLE_HID_API 1
    #else
        #define HAS_BLE_HID_API 0
    #endif
#else
    #define HAS_BLE_HID_API 0
#endif

#if HAS_BLE_HID_API
// Static storage for BLE HID profile instance and BT service
static FuriHalBleProfileBase* g_ble_hid_profile = NULL;
static Bt* g_bt_service = NULL;
#endif

#if HAS_BLE_HID_API
// BLE HID functions using extra_profiles/hid_profile.h API
static bool furi_hal_bt_hid_start(void) {
    // If already started, just return true
    if(g_ble_hid_profile && g_bt_service) {
        return true;
    }
    
    if(!g_bt_service) {
        g_bt_service = furi_record_open(RECORD_BT);
        if(!g_bt_service) {
            FURI_LOG_E(TAG, "Failed to open BT service");
            return false;
        }
    }
    
    // Disconnect any existing connection
    bt_disconnect(g_bt_service);
    furi_delay_ms(200);  // Wait for 2nd core to update
    
    // Start BLE HID profile
    // The profile should automatically set name to "Control <Flipper Name>"
    // based on firmware configuration
    g_ble_hid_profile = bt_profile_start(g_bt_service, ble_profile_hid, NULL);
    
    if(!g_ble_hid_profile) {
        FURI_LOG_E(TAG, "Failed to start BLE HID profile");
        return false;
    }
    
    // Give profile time to initialize
    furi_delay_ms(100);
    
    // Start advertising - device should appear as "Control <Flipper Name>"
    furi_hal_bt_start_advertising();
    
    // Give advertising time to start
    furi_delay_ms(100);
    
    return true;
}

static bool furi_hal_bt_hid_is_connected(void) {
    if(!g_bt_service || !g_ble_hid_profile) {
        return false;
    }
    // Check if BLE is active and profile is running
    // The profile being active means it's ready for connections
    // We can't directly check connection status, so we check if BLE is active
    return furi_hal_bt_is_active();
}

static void furi_hal_bt_hid_disconnect(void) {
    if(g_bt_service) {
        bt_disconnect(g_bt_service);
        furi_delay_ms(200);
    }
    furi_hal_bt_stop_advertising();
    
    if(g_ble_hid_profile && g_bt_service) {
        bt_profile_restore_default(g_bt_service);
        g_ble_hid_profile = NULL;
    }
    
    if(g_bt_service) {
        furi_record_close(RECORD_BT);
        g_bt_service = NULL;
    }
}
#endif

void deinitialize_hid_with_restore(FuriHalUsbInterface* previous_config) {
    deinitialize_hid_with_restore_and_mode(previous_config, HidModeUsb);
}

void deinitialize_hid_with_restore_and_mode(FuriHalUsbInterface* previous_config, HidMode mode) {
    #if HAS_BLE_HID_API
    if(mode == HidModeBle && g_ble_hid_profile) {
        // Release all keys using BLE profile
        ble_profile_hid_kb_release_all(g_ble_hid_profile);
    } else {
        // Release all keys using USB HID
        furi_hal_hid_kb_release_all();
    }
    #else
    furi_hal_hid_kb_release_all();
    #endif
    
    furi_delay_ms(HID_INIT_DELAY_MS);
    
    if(mode == HidModeBle) {
        // Deinitialize BLE HID
        furi_hal_bt_hid_disconnect();
    } else {
        // Deinitialize USB HID
        if(previous_config) {
            furi_hal_usb_set_config(previous_config, NULL);
        } else {
            furi_hal_usb_unlock();
        }
    }
    furi_delay_ms(HID_SETTLE_DELAY_MS);
}

bool initialize_hid_and_wait(void) {
    return initialize_hid_and_wait_with_mode(HidModeUsb);
}

bool initialize_hid_and_wait_with_mode(HidMode mode) {
    if(mode == HidModeBle) {
        // Initialize BLE HID
        // Set BLE device name to "Control <Flipper Name>" before starting profile
        const char* flipper_name = furi_hal_version_get_name_ptr();
        char ble_name[32];
        snprintf(ble_name, sizeof(ble_name), "Control %s", flipper_name ? flipper_name : "Flipper");
        
        // Set BLE app icon if available
        #ifdef BtIconHid
            furi_hal_bt_set_app_icon(BtIconHid);
        #endif
        
        // Start BLE HID (this also starts advertising)
        // Note: Name setting may need to be done through profile params or may be automatic
        if(!furi_hal_bt_hid_start()) {
            return false;
        }
        
        // Try to set name after profile start (some firmwares require this)
        // The profile may handle name automatically, but we try to set it explicitly
        (void)ble_name;  // Suppress unused warning for now
        
        // Wait for connection
        uint8_t retries = HID_CONNECT_TIMEOUT_MS / HID_CONNECT_RETRY_MS;
        for(uint8_t i = 0; i < retries; i++) {
            if(furi_hal_bt_hid_is_connected()) {
                return true;
            }
            furi_delay_ms(HID_CONNECT_RETRY_MS);
        }
        return furi_hal_bt_hid_is_connected();
    } else {
        // Initialize USB HID
        furi_hal_usb_unlock();
        if(!furi_hal_usb_set_config(&usb_hid, NULL)) {
            return false;
        }
        uint8_t retries = HID_CONNECT_TIMEOUT_MS / HID_CONNECT_RETRY_MS;
        for(uint8_t i = 0; i < retries; i++) {
            if(furi_hal_hid_is_connected()) {
                return true;
            }
            furi_delay_ms(HID_CONNECT_RETRY_MS);
        }
        return furi_hal_hid_is_connected();
    }
}

uint32_t app_type_password(App* app, const char* password) {
    if(!password) return 0;

    if(!app->layout_loaded) {
        app_load_keyboard_layout(app);
    }

    uint32_t approx_typed_ms = 0;
    #if HAS_BLE_HID_API
    bool use_ble = (app && app->hid_mode == HidModeBle);
    #endif

    for(size_t i = 0; password[i] != '\0'; i++) {
        unsigned char uc = (unsigned char)password[i];
        if(uc >= 128) continue;

        uint16_t full_keycode = app->layout[uc];
        if(full_keycode != HID_KEYBOARD_NONE) {
            #if HAS_BLE_HID_API
            if(use_ble && g_ble_hid_profile) {
                // Use BLE HID profile functions
                ble_profile_hid_kb_press(g_ble_hid_profile, full_keycode);
            } else {
                // Use USB HID functions
                furi_hal_hid_kb_press(full_keycode);
            }
            #else
            // USB HID only
            furi_hal_hid_kb_press(full_keycode);
            #endif
            
            furi_delay_ms(KEY_PRESS_DELAY_MS);
            
            #if HAS_BLE_HID_API
            if(use_ble && g_ble_hid_profile) {
                ble_profile_hid_kb_release(g_ble_hid_profile, full_keycode);
            } else {
                furi_hal_hid_kb_release(full_keycode);
            }
            #else
            furi_hal_hid_kb_release(full_keycode);
            #endif
            
            furi_delay_ms(KEY_RELEASE_DELAY_MS);
            approx_typed_ms += KEY_PRESS_DELAY_MS + KEY_RELEASE_DELAY_MS;

            uint16_t delay = app ? app->input_delay_ms : KEY_RELEASE_DELAY_MS;
            furi_delay_ms(delay);
            approx_typed_ms += delay;
        }
    }

    if(app && app->append_enter) {
        #if HAS_BLE_HID_API
        if(use_ble && g_ble_hid_profile) {
            ble_profile_hid_kb_press(g_ble_hid_profile, HID_KEYBOARD_RETURN);
        } else {
            furi_hal_hid_kb_press(HID_KEYBOARD_RETURN);
        }
        #else
        furi_hal_hid_kb_press(HID_KEYBOARD_RETURN);
        #endif
        
        furi_delay_ms(ENTER_PRESS_DELAY_MS);
        
        #if HAS_BLE_HID_API
        if(use_ble && g_ble_hid_profile) {
            ble_profile_hid_kb_release(g_ble_hid_profile, HID_KEYBOARD_RETURN);
        } else {
            furi_hal_hid_kb_release(HID_KEYBOARD_RETURN);
        }
        #else
        furi_hal_hid_kb_release(HID_KEYBOARD_RETURN);
        #endif
        
        furi_delay_ms(ENTER_RELEASE_DELAY_MS);
        approx_typed_ms += ENTER_PRESS_DELAY_MS + ENTER_RELEASE_DELAY_MS;
    }

    return approx_typed_ms;
}

void app_start_ble_advertising(void) {
    #if HAS_BLE_HID_API
    if(!g_bt_service) {
        g_bt_service = furi_record_open(RECORD_BT);
        if(!g_bt_service) {
            FURI_LOG_E(TAG, "Failed to open BT service");
            return;
        }
    }
    
    // Disconnect any existing connection
    bt_disconnect(g_bt_service);
    furi_delay_ms(200);
    
    // Start BLE HID profile
    g_ble_hid_profile = bt_profile_start(g_bt_service, ble_profile_hid, NULL);
    
    if(!g_ble_hid_profile) {
        FURI_LOG_E(TAG, "Failed to start BLE HID profile");
        return;
    }
    
    // Give profile time to initialize
    furi_delay_ms(100);
    
    // Start advertising
    furi_hal_bt_start_advertising();
    
    // Give advertising time to start
    furi_delay_ms(100);
    #else
    FURI_LOG_W(TAG, "BLE HID not available");
    #endif
}

void app_stop_ble_advertising(void) {
    #if HAS_BLE_HID_API
    if(g_bt_service) {
        bt_disconnect(g_bt_service);
        furi_delay_ms(200);
    }
    
    furi_hal_bt_stop_advertising();
    
    if(g_ble_hid_profile && g_bt_service) {
        bt_profile_restore_default(g_bt_service);
        g_ble_hid_profile = NULL;
    }
    
    if(g_bt_service) {
        furi_record_close(RECORD_BT);
        g_bt_service = NULL;
    }
    #endif
}