#include <iostream>
#include <string>
#include <vector>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <cerrno>
#include <cstring>
#include <csignal>
#include <dirent.h>
#include <algorithm> // Required for std::min/std::max
#include <cmath> // Required for round()
#include <chrono>
using namespace std::chrono;

#include <linux/input.h>
#include <sys/ioctl.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <bluetooth/hci_lib.h>
#include <bluetooth/l2cap.h>
#include <bluetooth/sdp.h>
#include <bluetooth/sdp_lib.h>

// HID report descriptor and service record details
#define PSMHIDCTL	0x11
#define	PSMHIDINT	0x13
#define	HIDINFO_NAME	"Woolly HID Emulator"
#define	HIDINFO_PROV	"Woolly"
#define	HIDINFO_DESC	"Keyboard and Mouse"
#define	REPORTID_MOUSE	1
#define	REPORTID_KEYBD	2
#define SDPRECORD	"\x05\x01\x09\x02\xA1\x01\x85\x01\x09\x01\xA1\x00" \
			"\x05\x09\x19\x01\x29\x03\x15\x00\x25\x01\x75\x01" \
			"\x95\x03\x81\x02\x75\x05\x95\x01\x81\x03\x05\x01" \
			"\x09\x30\x09\x31\x09\x38\x15\x81\x25\x7F\x75\x08" \
			"\x95\x03\x81\x06\xC0\xC0\x05\x01\x09\x06\xA1\x01" \
			"\x85\x02\xA1\x00\x05\x07\x19\xE0\x29\xE7\x15\x00" \
			"\x25\x01\x75\x01\x95\x08\x81\x02\x95\x08\x75\x08" \
			"\x15\x00\x25\x65\x05\x07\x19\x00\x29\x65\x81\x00" \
			"\xC0\xC0"
#define SDPRECORD_BYTES	98

// Globals
volatile bool keep_running = true;
int ctl_sock = -1, int_sock = -1;
int ctl_conn = -1, int_conn = -1;
sdp_session_t *sdp_session = nullptr;
std::vector<int> event_fds;

// HID report structures
struct hidrep_mouse_t {
	unsigned char	btcode;
	unsigned char	rep_id;
	unsigned char	button;
	signed   char	axis_x;
	signed   char	axis_y;
	signed   char	axis_z;
} __attribute__((packed));

struct hidrep_keyb_t {
	unsigned char	btcode;
	unsigned char	rep_id;
	unsigned char	modify;
	unsigned char	key[8];
} __attribute__((packed));

// State for HID reports
char mousebuttons = 0;
char modifierkeys = 0;
char pressedkey[8] = { 0, 0, 0, 0,  0, 0, 0, 0 };
double dx = 0.0, dy = 0.0, dz = 0.0; // Use double for accumulators to maintain precision

void signal_handler(int signum) {
    std::cout << "\nCaught signal " << signum << ", shutting down." << std::endl;
    keep_running = false;
    // Forcefully close listening sockets to unblock accept() calls
    if (ctl_sock >= 0) close(ctl_sock);
    if (int_sock >= 0) close(int_sock);
    ctl_sock = -1;
    int_sock = -1;
}

// HACK from hidclient.c, required for sdp_record_register to not segfault
sdp_data_t *sdp_seq_alloc_with_length(void **dtds, void **values, int *length, int len) {
    sdp_data_t *curr = NULL, *seq = NULL;
    int i;
    int totall = 1024;
    for (i = 0; i < len; i++) {
        sdp_data_t *data;
        int8_t dtd = *(uint8_t *) dtds[i];
        if (dtd >= SDP_SEQ8 && dtd <= SDP_ALT32) {
            data = (sdp_data_t *) values[i];
        } else {
            data = sdp_data_alloc_with_length(dtd, values[i], length[i]);
        }
        if (!data) return NULL;
        if (curr) curr->next = data;
        else seq = data;
        curr = data;
        totall +=  length[i] + sizeof *seq;
    }
    return sdp_data_alloc_with_length(SDP_SEQ8, seq, totall);
}

// Creates the SDP record for the HID service
sdp_session_t *register_hid_service() {
    sdp_record_t record;
    memset(&record, 0, sizeof(sdp_record_t));
    record.handle = 0xffffffff;

    sdp_list_t *svclass_id, *pfseq, *apseq, *root;
    uuid_t root_uuid, hidkb_uuid, l2cap_uuid, hidp_uuid;
    sdp_profile_desc_t profile[1];
    sdp_list_t *aproto, *proto[3];
    sdp_data_t *psm, *lang_lst, *lang_lst2, *hid_spec_lst, *hid_spec_lst2;
    void *dtds[2], *values[2], *dtds2[2], *values2[2];
    int i, leng[2];
    uint8_t dtd = SDP_UINT16, dtd2 = SDP_UINT8, dtd_data = SDP_TEXT_STR8, hid_spec_type = 0x22;
    uint16_t hid_attr_lang[] = {0x409, 0x100}, ctrl = PSMHIDCTL, intr = PSMHIDINT, hid_attr[] = {0x100, 0x111, 0x40, 0x00, 0x01, 0x01}, hid_attr2[] = {0x100, 0x0};

    bdaddr_t bdaddr_any = {{0,0,0,0,0,0}};
    bdaddr_t bdaddr_local = {{0,0,0,0xff,0xff,0xff}};
    sdp_session_t *session = sdp_connect(&bdaddr_any, &bdaddr_local, 0);
    if (!session) {
        std::cerr << "Failed to connect to SDP server: " << strerror(errno) << std::endl;
        return nullptr;
    }

    sdp_uuid16_create(&root_uuid, PUBLIC_BROWSE_GROUP);
    root = sdp_list_append(0, &root_uuid);
    sdp_set_browse_groups(&record, root);

    sdp_uuid16_create(&hidkb_uuid, HID_SVCLASS_ID);
    svclass_id = sdp_list_append(0, &hidkb_uuid);
    sdp_set_service_classes(&record, svclass_id);

    sdp_uuid16_create(&profile[0].uuid, HID_PROFILE_ID);
    profile[0].version = 0x0100;
    pfseq = sdp_list_append(0, profile);
    sdp_set_profile_descs(&record, pfseq);

    sdp_uuid16_create(&l2cap_uuid, L2CAP_UUID);
    proto[1] = sdp_list_append(0, &l2cap_uuid);
    psm = sdp_data_alloc(SDP_UINT16, &ctrl);
    proto[1] = sdp_list_append(proto[1], psm);
    apseq = sdp_list_append(0, proto[1]);

    sdp_uuid16_create(&hidp_uuid, HIDP_UUID);
    proto[2] = sdp_list_append(0, &hidp_uuid);
    apseq = sdp_list_append(apseq, proto[2]);
    aproto = sdp_list_append(0, apseq);
    sdp_set_access_protos(&record, aproto);

    proto[1] = sdp_list_append(0, &l2cap_uuid);
    psm = sdp_data_alloc(SDP_UINT16, &intr);
    proto[1] = sdp_list_append(proto[1], psm);
    apseq = sdp_list_append(0, proto[1]);
    sdp_uuid16_create(&hidp_uuid, HIDP_UUID);
    proto[2] = sdp_list_append(0, &hidp_uuid);
    apseq = sdp_list_append(apseq, proto[2]);
    aproto = sdp_list_append(0, apseq);
    sdp_set_add_access_protos(&record, aproto);

    sdp_set_info_attr(&record, HIDINFO_NAME, HIDINFO_PROV, HIDINFO_DESC);

    sdp_attr_add_new(&record, SDP_ATTR_HID_DEVICE_RELEASE_NUMBER, SDP_UINT16, &hid_attr[0]);
    sdp_attr_add_new(&record, SDP_ATTR_HID_PARSER_VERSION, SDP_UINT16, &hid_attr[1]);
    sdp_attr_add_new(&record, SDP_ATTR_HID_DEVICE_SUBCLASS, SDP_UINT8, &hid_attr[2]);
    sdp_attr_add_new(&record, SDP_ATTR_HID_COUNTRY_CODE, SDP_UINT8, &hid_attr[3]);
    sdp_attr_add_new(&record, SDP_ATTR_HID_VIRTUAL_CABLE, SDP_BOOL, &hid_attr[4]);
    sdp_attr_add_new(&record, SDP_ATTR_HID_RECONNECT_INITIATE, SDP_BOOL, &hid_attr[5]);

    dtds[0] = &dtd2;
    values[0] = &hid_spec_type;
    dtd_data = SDPRECORD_BYTES <= 255 ? SDP_TEXT_STR8 : SDP_TEXT_STR16;
    dtds[1] = &dtd_data;
    values[1] = (uint8_t *)SDPRECORD;
    leng[0] = 0;
    leng[1] = SDPRECORD_BYTES;
    hid_spec_lst = sdp_seq_alloc_with_length(dtds, values, leng, 2);
    hid_spec_lst2 = sdp_data_alloc(SDP_SEQ8, hid_spec_lst);
    sdp_attr_add(&record, SDP_ATTR_HID_DESCRIPTOR_LIST, hid_spec_lst2);

    for (i = 0; i < sizeof(hid_attr_lang) / 2; i++) {
        dtds2[i] = &dtd;
        values2[i] = &hid_attr_lang[i];
    }
    lang_lst = sdp_seq_alloc(dtds2, values2, sizeof(hid_attr_lang) / 2);
    lang_lst2 = sdp_data_alloc(SDP_SEQ8, lang_lst);
    sdp_attr_add(&record, SDP_ATTR_HID_LANG_ID_BASE_LIST, lang_lst2);

    sdp_attr_add_new(&record, SDP_ATTR_HID_PROFILE_VERSION, SDP_UINT16, &hid_attr2[0]);
    sdp_attr_add_new(&record, SDP_ATTR_HID_BOOT_DEVICE, SDP_UINT16, &hid_attr2[1]);

    if (sdp_record_register(session, &record, SDP_RECORD_PERSIST) < 0) {
        std::cerr << "Service Record registration failed: " << strerror(errno) << std::endl;
        sdp_close(session);
        return nullptr;
    }

    std::cout << "HID keyboard/mouse service registered." << std::endl;
    return session;
}

void close_event_devices() {
    for (int fd : event_fds) {
        if (fd >= 0) {
            ioctl(fd, EVIOCGRAB, 0); // Release grab
            close(fd);
        }
    }
    event_fds.clear();
    std::cout << "Released input devices." << std::endl;
}



int init_event_devices() {
    // Open and grab selected devices
    std::cout << "Delaying 2 seconds before grabbing devices..." << std::endl;
    sleep(2);

    const std::string input_dir = "/dev/input/";
    DIR *dir = opendir(input_dir.c_str());
    if (!dir) {
        std::cerr << "Could not open /dev/input/. Is this a Linux system?" << std::endl;
        return -1;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (strncmp(entry->d_name, "event", 5) != 0) {
            continue;
        }

        std::string path = input_dir + entry->d_name;
        int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK);
        if (fd < 0) {
            continue;
        }

        char name[256] = "Unknown";
        ioctl(fd, EVIOCGNAME(sizeof(name)), name);

        // Check if the device name contains "keyboard" or "mouse"
        if (strstr(name, "keyboard") != nullptr || strstr(name, "Keyboard") != nullptr) {
            unsigned long ev_bits = 0;
            ioctl(fd, EVIOCGBIT(0, sizeof(ev_bits)), &ev_bits);
            if ((1UL << EV_KEY) & ev_bits) {
                if (ioctl(fd, EVIOCGRAB, 1) == 0) {
                    event_fds.push_back(fd);
                    std::cout << "Grabbed keyboard: " << path << " (" << name << ")" << std::endl;
                } else {
                    std::cerr << "Could not grab keyboard device " << path << ". Are you root?" << std::endl;
                    close(fd);
                }
            } else {
                close(fd);
            }
        } else if (strstr(name, "mouse") != nullptr || strstr(name, "Mouse") != nullptr) {
            unsigned long ev_bits = 0;
            ioctl(fd, EVIOCGBIT(0, sizeof(ev_bits)), &ev_bits);
            if ((1UL << EV_REL) & ev_bits) {
                if (ioctl(fd, EVIOCGRAB, 1) == 0) {
                    event_fds.push_back(fd);
                    std::cout << "Grabbed mouse: " << path << " (" << name << ")" << std::endl;
                } else {
                    std::cerr << "Could not grab mouse device " << path << ". Are you root?" << std::endl;
                    close(fd);
                }
            } else {
                close(fd);
            }
        } else {
            close(fd);
        }
    }
    closedir(dir);

    if (event_fds.empty()) {
        std::cerr << "No suitable keyboard or mouse found in /dev/input/" << std::endl;
        return -1;
    }

    return event_fds.size();
}

unsigned char map_key_to_hid(int code) {
    switch (code) {
        case KEY_A: return 4;
        case KEY_B: return 5;
        case KEY_C: return 6;
        case KEY_D: return 7;
        case KEY_E: return 8;
        case KEY_F: return 9;
        case KEY_G: return 10;
        case KEY_H: return 11;
        case KEY_I: return 12;
        case KEY_J: return 13;
        case KEY_K: return 14;
        case KEY_L: return 15;
        case KEY_M: return 16;
        case KEY_N: return 17;
        case KEY_O: return 18;
        case KEY_P: return 19;
        case KEY_Q: return 20;
        case KEY_R: return 21;
        case KEY_S: return 22;
        case KEY_T: return 23;
        case KEY_U: return 24;
        case KEY_V: return 25;
        case KEY_W: return 26;
        case KEY_X: return 27;
        case KEY_Y: return 28;
        case KEY_Z: return 29;
        case KEY_1: return 30;
        case KEY_2: return 31;
        case KEY_3: return 32;
        case KEY_4: return 33;
        case KEY_5: return 34;
        case KEY_6: return 35;
        case KEY_7: return 36;
        case KEY_8: return 37;
        case KEY_9: return 38;
        case KEY_0: return 39;
        case KEY_ENTER: return 40;
        case KEY_ESC: return 41;
        case KEY_BACKSPACE: return 42;
        case KEY_TAB: return 43;
        case KEY_SPACE: return 44;
        case KEY_MINUS: return 45;
        case KEY_EQUAL: return 46;
        case KEY_LEFTBRACE: return 47;
        case KEY_RIGHTBRACE: return 48;
        case KEY_BACKSLASH: return 49;
        case KEY_SEMICOLON: return 51;
        case KEY_APOSTROPHE: return 52;
        case KEY_GRAVE: return 53;
        case KEY_COMMA: return 54;
        case KEY_DOT: return 55;
        case KEY_SLASH: return 56;
        case KEY_CAPSLOCK: return 57;
        case KEY_F1: return 58;
        case KEY_F2: return 59;
        case KEY_F3: return 60;
        case KEY_F4: return 61;
        case KEY_F5: return 62;
        case KEY_F6: return 63;
        case KEY_F7: return 64;
        case KEY_F8: return 65;
        case KEY_F9: return 66;
        case KEY_F10: return 67;
        case KEY_F11: return 68;
        case KEY_F12: return 69;
        case KEY_SYSRQ: return 70;
        case KEY_SCROLLLOCK: return 71;
        case KEY_PAUSE: return 72;
        case KEY_INSERT: return 73;
        case KEY_HOME: return 74;
        case KEY_PAGEUP: return 75;
        case KEY_DELETE: return 76;
        case KEY_END: return 77;
        case KEY_PAGEDOWN: return 78;
        case KEY_RIGHT: return 79;
        case KEY_LEFT: return 80;
        case KEY_DOWN: return 81;
        case KEY_UP: return 82;
        case KEY_NUMLOCK: return 83;
        case KEY_KPSLASH: return 84;
        case KEY_KPASTERISK: return 85;
        case KEY_KPMINUS: return 86;
        case KEY_KPPLUS: return 87;
        case KEY_KPENTER: return 88;
        case KEY_KP1: return 89;
        case KEY_KP2: return 90;
        case KEY_KP3: return 91;
        case KEY_KP4: return 92;
        case KEY_KP5: return 93;
        case KEY_KP6: return 94;
        case KEY_KP7: return 95;
        case KEY_KP8: return 96;
        case KEY_KP9: return 97;
        case KEY_KP0: return 98;
        case KEY_KPDOT: return 99;
        default: return 0;
    }
}

void process_one_event(struct input_event *inevent) {
    hidrep_mouse_t evmouse;
    hidrep_keyb_t evkeyb;
    int j;

    switch (inevent->type) {
        case EV_KEY: {
            unsigned int u = 1;
            switch (inevent->code) {
                case BTN_LEFT:
                case BTN_RIGHT:
                case BTN_MIDDLE: {
                    char c = 1 << (inevent->code & 0x03);
                    mousebuttons &= (0x07 - c); 
                    if (inevent->value == 1) mousebuttons |= c;

                    // --- ADD THIS BLOCK ---
                    // Send a mouse report immediately to register the click.
                    // This report has the new button state but zero movement.
                    hidrep_mouse_t evmouse;
                    evmouse.btcode = 0xA1;
                    evmouse.rep_id = REPORTID_MOUSE;
                    evmouse.button = mousebuttons & 0x07;
                    evmouse.axis_x = 0;
                    evmouse.axis_y = 0;
                    evmouse.axis_z = 0;
                    if (send(int_conn, &evmouse, sizeof(evmouse), MSG_NOSIGNAL) < 0) {
                        keep_running = false;
                    }
                    // --- END OF ADDED BLOCK ---

                    break;
                }
                case KEY_RIGHTMETA: u <<= 1;
                case KEY_RIGHTALT: u <<= 1;
                case KEY_RIGHTSHIFT: u <<= 1;
                case KEY_RIGHTCTRL: u <<= 1;
                case KEY_LEFTMETA: u <<= 1;
                case KEY_LEFTALT: u <<= 1;
                case KEY_LEFTSHIFT: u <<= 1;
                case KEY_LEFTCTRL: {
                    modifierkeys &= (0xff - u);
                    if (inevent->value >= 1) modifierkeys |= u;
                    evkeyb.btcode = 0xA1;
                    evkeyb.rep_id = REPORTID_KEYBD;
                    memcpy(evkeyb.key, pressedkey, 8);
                    evkeyb.modify = modifierkeys;
                    if (send(int_conn, &evkeyb, sizeof(evkeyb), MSG_NOSIGNAL) < 0) keep_running = false;
                    break;
                }
                default: {
                    unsigned char hid_code = map_key_to_hid(inevent->code);
                    if (hid_code != 0) {
                        if (inevent->value == 1) { // Key Down
                            for (j = 0; j < 8; ++j) if (pressedkey[j] == 0) { pressedkey[j] = hid_code; break; }
                        } else if (inevent->value == 0) { // Key Up
                            for (j = 0; j < 8; ++j) if (pressedkey[j] == hid_code) {
                                for (int k = j; k < 7; ++k) pressedkey[k] = pressedkey[k+1];
                                pressedkey[7] = 0;
                                break;
                            }
                        }
                    }
                    
                    evkeyb.btcode = 0xA1;
                    evkeyb.rep_id = REPORTID_KEYBD;
                    memcpy(evkeyb.key, pressedkey, 8);
                    evkeyb.modify = modifierkeys;
                    if (send(int_conn, &evkeyb, sizeof(evkeyb), MSG_NOSIGNAL) < 0) keep_running = false;
                    break;
                }
            }
            break;
        }
        case EV_REL: {
            if (inevent->code == REL_X) dx += inevent->value;
            if (inevent->code == REL_Y) dy += inevent->value;
            if (inevent->code == REL_WHEEL) dz += inevent->value;
            break;
        }
    }
}

void send_pending_reports(int int_sock) {
    hidrep_mouse_t evmouse;
    evmouse.btcode = 0xA1;
    evmouse.rep_id = REPORTID_MOUSE;
    evmouse.button = mousebuttons & 0x07;

    // Loop to send multiple reports if accumulated movement is large
    // Continue as long as there's significant movement remaining
    while (abs(dx) >= 1.0 || abs(dy) >= 1.0 || abs(dz) >= 1.0) {
        // Calculate the payload for this report, clamping to signed char limits
        signed char report_payload_x = static_cast<signed char>(std::max(-127.0, std::min(127.0, floor(dx))));
        signed char report_payload_y = static_cast<signed char>(std::max(-127.0, std::min(127.0, floor(dy))));
        signed char report_payload_z = static_cast<signed char>(std::max(-127.0, std::min(127.0, floor(dz))));

        // If all payloads are zero, but there's still accumulated movement,
        // it means the remaining movement is sub-pixel and less than 0.5.
        // In this case, we break to avoid an infinite loop.
        if (report_payload_x == 0 && report_payload_y == 0 && report_payload_z == 0) {
            break;
        }

        evmouse.axis_x = report_payload_x;
        evmouse.axis_y = report_payload_y;
        evmouse.axis_z = report_payload_z;

        if (send(int_sock, &evmouse, sizeof(evmouse), MSG_NOSIGNAL) < 0) {
            keep_running = false;
            return; // Exit if send fails
        }

        // Subtract the sent values from the accumulators
        dx -= report_payload_x;
        dy -= report_payload_y;
        dz -= report_payload_z;
    }
}

int main() {
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    sdp_session = register_hid_service();
    if (!sdp_session) {
        std::cerr << "Failed to register SDP service. Is bluetoothd running?" << std::endl;
        return 1;
    }

    ctl_sock = socket(AF_BLUETOOTH, SOCK_SEQPACKET, BTPROTO_L2CAP);
    int_sock = socket(AF_BLUETOOTH, SOCK_SEQPACKET, BTPROTO_L2CAP);

    if (ctl_sock < 0 || int_sock < 0) {
        std::cerr << "Error creating sockets: " << strerror(errno) << std::endl;
        return 1;
    }

    struct sockaddr_l2 addr = { 0 };
    addr.l2_family = AF_BLUETOOTH;
    addr.l2_psm = htobs(PSMHIDCTL);
    bdaddr_t bdaddr_any = {0,0,0,0,0,0};
    bacpy(&addr.l2_bdaddr, &bdaddr_any);

    if (bind(ctl_sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        std::cerr << "Error binding control socket: " << strerror(errno) << std::endl;
        return 1;
    }

    addr.l2_psm = htobs(PSMHIDINT);
    if (bind(int_sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        std::cerr << "Error binding interrupt socket: " << strerror(errno) << std::endl;
        return 1;
    }

    if (listen(ctl_sock, 1) < 0 || listen(int_sock, 1) < 0) {
        std::cerr << "Error listening on sockets: " << strerror(errno) << std::endl;
        return 1;
    }
    std::cout << "Waiting for connections..." << std::endl;

    auto last_report_time = steady_clock::now();
    const milliseconds report_interval(1); // For 500Hz, use 1 for 1000Hz

    while (keep_running) {
        struct sockaddr_l2 rem_addr = { 0 };
        socklen_t opt = sizeof(rem_addr);

        ctl_conn = accept(ctl_sock, (struct sockaddr *)&rem_addr, &opt);
        if (ctl_conn < 0) { if (errno == EINTR || !keep_running) break; continue; }

        char addr_str[18];
        ba2str(&rem_addr.l2_bdaddr, addr_str);
        std::cout << "Control connection accepted from " << addr_str << std::endl;

        int_conn = accept(int_sock, (struct sockaddr *)&rem_addr, &opt);
        if (int_conn < 0) { close(ctl_conn); ctl_conn = -1; continue; }
        std::cout << "Interrupt connection accepted from " << addr_str << std::endl;
        
        if (init_event_devices() <= 0) {
            std::cerr << "Could not grab any input devices. Closing connection." << std::endl;
            close(ctl_conn);
            close(int_conn);
            ctl_conn = -1;
            int_conn = -1;
            continue;
        }

        std::cout << "\nSuccessfully connected! Forwarding inputs..." << std::endl;

        // Main event loop
        while (keep_running) {
            struct pollfd fds[event_fds.size() + 2];
            for(size_t i = 0; i < event_fds.size(); ++i) {
                fds[i].fd = event_fds[i];
                fds[i].events = POLLIN;
            }
            fds[event_fds.size()].fd = ctl_conn;
            fds[event_fds.size()].events = POLLIN;
            fds[event_fds.size() + 1].fd = int_conn;
            fds[event_fds.size() + 1].events = POLLIN;

            int ret = poll(fds, event_fds.size() + 2, 1); // Reduced timeout for smoother mouse
            if (ret < 0) { if (errno == EINTR) continue; break; }

            for (size_t i = 0; i < event_fds.size(); ++i) {
                if (fds[i].revents & POLLIN) {
                    struct input_event inevent;
                    if (read(fds[i].fd, &inevent, sizeof(inevent)) > 0) {
                        process_one_event(&inevent);
                    }
                }
            }

            

            if (fds[event_fds.size()].revents & (POLLHUP | POLLERR) || fds[event_fds.size() + 1].revents & (POLLHUP | POLLERR)) {
                std::cout << "Connection lost." << std::endl;
                break;
            }

            // NEW: Timer-based report sending logic
            auto current_time = steady_clock::now();
            if (duration_cast<milliseconds>(current_time - last_report_time) >= report_interval) {
                send_pending_reports(int_conn);
                last_report_time = current_time;
            }
        }

        close_event_devices();
        if (ctl_conn >= 0) close(ctl_conn);
        if (int_conn >= 0) close(int_conn);
        ctl_conn = -1;
        int_conn = -1;
        std::cout << "Connection closed. Waiting for new connection..." << std::endl;
    }

    std::cout << "\nClosing listening sockets and cleaning up." << std::endl;
    if (ctl_sock >= 0) close(ctl_sock);
    if (int_sock >= 0) close(int_sock);
    if (sdp_session) sdp_close(sdp_session);

    return 0;
}
