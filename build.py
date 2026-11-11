"""Build the ARMv7 receiver and Debian package with Clang, ld64 and ldid."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import io
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import tarfile


ROOT = Path(__file__).resolve().parent
BTSTACK_REV = "924daf9e4265a6459f04c3acfdb0b615fe2854e4"


def run(*args, **kwargs):
    return subprocess.run([str(arg) for arg in args], check=True, **kwargs)


def toolpath(path):
    path = Path(path).resolve()
    if path.drive:
        return "/cygdrive/" + path.drive[0].lower() + path.as_posix()[2:]
    return str(path)


def patch_demo(btstack, build_dir):
    demo = (btstack / "example/a2dp_sink_demo.c").read_text()
    demo = '#include "media_info.h"\n' + demo
    demo = demo.replace('gap_set_local_name("A2DP Sink Demo 00:00:00:00:00:00");',
                        'gap_set_local_name("DockBridge 4S");')
    demo = demo.replace("    gap_discoverable_control(1);", "")
    demo, count = re.subn(
        r'    if \(hci_event_packet_get_type\(packet\) == HCI_EVENT_PIN_CODE_REQUEST\) \{.*?\n    }\n',
        '    UNUSED(packet);\n', demo, flags=re.S)
    if count != 1:
        raise ValueError("Unexpected upstream pairing code")
    demo = demo[:demo.index("int btstack_main(int argc, const char * argv[]);")]
    demo += "\nint dockbridge_start_a2dp(void) { return a2dp_and_avrcp_setup(); }\n"
    # Restrict the old demo's fixed-width audio backend to stereo modes.
    demo = demo.replace("media_sbc_codec_capabilities[] = { 0xFF,",
                        "media_sbc_codec_capabilities[] = { 0x33,")
    demo = demo.replace("    sbc_frame_size = (size-pos)/ sbc_header.num_frames;", """
    if (!media_initialized || (packet[0] >> 6) != 2 || (packet[0] & 0x3f) ||
        sbc_header.fragmentation || !sbc_header.num_frames ||
        (size - pos) % sbc_header.num_frames) return;
    unsigned frame_size = (size - pos) / sbc_header.num_frames;
    if (frame_size < 4 || frame_size > MAX_SBC_FRAME_SIZE) return;
    for (unsigned i = 0; i < sbc_header.num_frames; ++i) {
        if (packet[pos + i * frame_size] != 0x9c) return;
    }
    if (sbc_frame_size != frame_size) {
        btstack_ring_buffer_init(&sbc_frame_ring_buffer, sbc_frame_storage, sizeof(sbc_frame_storage));
        sbc_frame_size = frame_size;
    }""")
    demo = demo.replace("            dump_sbc_configuration(sbc_configuration);",
                        "            dump_sbc_configuration(sbc_configuration);\n"
                        "            dockbridge_media_format(sbc_configuration.sampling_frequency,\n"
                        "                                    sbc_configuration.num_channels);")
    demo = demo.replace("            a2dp_cid = a2dp_subevent_stream_established_get_a2dp_cid(packet);",
                        "            a2dp_cid = a2dp_subevent_stream_established_get_a2dp_cid(packet);\n"
                        "            dockbridge_media_audio_connected(address);")
    for event in ("STREAM_RELEASED", "SIGNALING_CONNECTION_RELEASED"):
        anchor = f"        case A2DP_SUBEVENT_{event}:\n"
        assert demo.count(anchor) == 1
        getter = f"a2dp_subevent_{event.lower()}_get_a2dp_cid"
        demo = demo.replace(anchor, anchor + f"            if (a2dp_cid && {getter}(packet) != a2dp_cid) break;\n"
                            "            dockbridge_media_audio_disconnected();\n")
    demo = demo.replace("            avrcp_subevent_connection_established_get_bd_addr(packet, adress);",
                        "            avrcp_subevent_connection_established_get_bd_addr(packet, adress);\n"
                        "            dockbridge_media_avrcp_connected(local_cid, adress);")
    demo = demo.replace("        case AVRCP_SUBEVENT_CONNECTION_RELEASED:\n",
                        "        case AVRCP_SUBEVENT_CONNECTION_RELEASED:\n"
                        "            dockbridge_media_avrcp_disconnected(avrcp_subevent_connection_released_get_avrcp_cid(packet));\n")
    start = demo.index("static void avrcp_controller_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size){")
    end = demo.index("\nstatic void", start + 1)
    demo = demo[:start] + """static void avrcp_controller_packet_handler(uint8_t packet_type, uint16_t channel,
                                            uint8_t *packet, uint16_t size){
    UNUSED(channel);
    dockbridge_media_avrcp_event(packet_type, packet, size);
}
""" + demo[end:]
    demo = demo.replace("static uint8_t  avrcp_subevent_value[100];\n", "")
    target = build_dir / "a2dp_receiver.c"
    target.write_text(demo)
    return target


def patch_avrcp_controller(btstack, build_dir):
    original = btstack / "src/classic/avrcp_controller.c"
    source = original.read_text()
    start = source.index("static void avrcp_controller_emit_now_playing_info_event(")
    end = source.index("static void avrcp_controller_emit_operation_status", start)
    emitter = source[start:end]
    copy = "            (void)memcpy(event + pos, value, value_len);"
    assert emitter.count(copy) == 4
    emitter = emitter.replace(copy, "            if (value_len) memcpy(event + pos, value, value_len);\n"
                                     "            pos += value_len;")
    emitter = emitter.replace("btstack_atoi((char *)value)", "metadata_decimal(value, value_len)")
    emitter = emitter.replace("        default:\n            break;", "        default:\n            return;")
    source = source[:start] + """
static uint32_t metadata_decimal(const uint8_t *value, uint16_t size){
    uint32_t result = 0;
    for (uint16_t i = 0; i < size; ++i){
        if (value[i] < '0' || value[i] > '9') return 0;
        uint32_t digit = value[i] - '0';
        if (result > (UINT32_MAX - digit) / 10) return 0;
        result = result * 10 + digit;
    }
    return result;
}

""" + emitter + source[end:]
    source = source.replace("    connection->num_parsed_attributes = 0;\n",
                            "    connection->num_parsed_attributes = 0;\n"
                            "    connection->attribute_value_offset = 0;\n")
    source = source.replace("    if (connection->list_offset < connection->list_size){",
                            "    if (++connection->num_parsed_attributes < connection->num_attributes){")
    source = source.replace("    for (i=0;i<num_bytes_to_read;i++){",
                            "    for (i=0; i<num_bytes_to_read && connection->num_attributes; i++){")
    source = source.replace("            // Company ID (3)\n            pos += 3;",
                            "            if (size - pos < 7) return;\n"
                            "            // Company ID (3)\n            pos += 3;")
    source = source.replace("                case AVRCP_PDU_ID_GET_PLAY_STATUS:{",
                            "                case AVRCP_PDU_ID_GET_PLAY_STATUS:{\n"
                            "                    if (ctype != AVRCP_CTYPE_RESPONSE_IMPLEMENTED_STABLE || param_length < 9) break;")
    source = source.replace("                case AVRCP_PDU_ID_GET_ELEMENT_ATTRIBUTES:{",
                            """                case AVRCP_PDU_ID_GET_ELEMENT_ATTRIBUTES:{
                    if (ctype != AVRCP_CTYPE_RESPONSE_IMPLEMENTED_STABLE) {
                        avrcp_parser_reset(connection);
                        avrcp_controller_emit_now_playing_info_event_done(avrcp_controller_context.avrcp_callback,
                                                                         connection->avrcp_cid, ctype, 1);
                        break;
                    }""")
    source = source.replace("""                            connection->list_size = param_length;
                            connection->num_attributes = packet[pos++];

                            avrcp_controller_parse_and_emit_element_attrs(packet+pos, size-pos, connection, ctype);
                            if (vendor_dependent_packet_type == AVRCP_START_PACKET){""", """                            if (!param_length) break;
                            connection->num_attributes = packet[pos++];
                            if (!connection->num_attributes) {
                                avrcp_controller_emit_now_playing_info_event_done(avrcp_controller_context.avrcp_callback,
                                                                                 connection->avrcp_cid, ctype, 0);
                                break;
                            }
                            avrcp_controller_parse_and_emit_element_attrs(packet+pos, param_length-1, connection, ctype);
                            if (vendor_dependent_packet_type == AVRCP_START_PACKET && connection->num_attributes){""")
    source = source.replace("avrcp_controller_parse_and_emit_element_attrs(packet+pos, size-pos, connection, ctype);",
                            "avrcp_controller_parse_and_emit_element_attrs(packet+pos, param_length, connection, ctype);")
    source = source.replace("if (vendor_dependent_packet_type == AVRCP_CONTINUE_PACKET){",
                            "if (vendor_dependent_packet_type == AVRCP_CONTINUE_PACKET && connection->num_attributes){")
    source = source.replace("big_endian_store_16(connection->cmd_operands, pos, 9);",
                            "big_endian_store_16(connection->cmd_operands, pos, 9 + 4 * num_attributes);")
    target = build_dir / original.name
    target.write_text(source)
    return target


def patch_hci(btstack, build_dir):
    original = btstack / "src/hci.c"
    source = original.read_text()
    anchor = "// assumption: hci_can_send_command_packet_now() == true\nstatic void hci_initializing_run(void)"
    assert source.count(anchor) == 1, "Unexpected BTstack HCI initialization code"
    helper = """// These commands build their payload directly in the reserved HCI buffer.
static void hci_init_send_prepared_command(uint8_t *packet, int size){
    int result = hci_send_cmd_packet(packet, size);
    if (result < 0 || hci_transport_synchronous()){
        hci_release_packet_buffer();
    }
}

"""
    source = source.replace(anchor, helper + anchor)
    for length in ("DEVICE_NAME_LEN", "1 + EXTENDED_INQUIRY_RESPONSE_DATA_LEN"):
        call = f"hci_send_cmd_packet(packet, HCI_CMD_HEADER_SIZE + {length});"
        assert source.count(call) == 1, f"Unexpected BTstack HCI command for {length}"
        source = source.replace(call, call.replace("hci_send_cmd_packet", "hci_init_send_prepared_command"))
    target = build_dir / original.name
    target.write_text(source)
    return target


def patch_transport(btstack, build_dir):
    # The original transport computes the native baud but calls cfsetspeed with zero.
    original = btstack / "port/ios/src/hci_transport_h4_iphone.c"
    transport = '#include "bridge_ipc.h"\n' + original.read_text().replace(
        "speed_t brate = (speed_t) hci_transport_config_uart->baudrate_init;",
        "speed_t brate = (speed_t) baud;")
    transport = transport.replace("    if (!hci_transport_h4->ds) return -1;",
                                  "    if (!hci_transport_h4->ds) goto err_out4;", 1)
    transport = transport.replace("static int h4_close(void){",
                                  "static int h4_close(void){\n    if (!hci_transport_h4->ds) return 0;")
    start = transport.index("static int h4_send_packet(uint8_t packet_type, uint8_t * packet, int size){")
    end = transport.index("static void   h4_register_packet_handler", start)
    transport = transport[:start] + """
static int h4_write_all(const uint8_t *data, size_t size) {
    unsigned retries = 20;
    while (size) {
        ssize_t count = write(hci_transport_h4->uart_fd, data, size);
        if (count > 0) {
            data += count;
            size -= (size_t)count;
            continue;
        }
        if (count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) && retries) {
            --retries;
            usleep(5000);
            continue;
        }
        if (count == 0) errno = EIO;
        return -1;
    }
    return 0;
}

static int h4_send_packet(uint8_t packet_type, uint8_t *packet, int size) {
    if (!hci_transport_h4->ds || size <= 0) return -1;
    int trace = dockbridge_trace_hci_init() && packet_type == HCI_COMMAND_DATA_PACKET && size >= 3;
    if (trace) fprintf(stderr, "HCI init > 0x%04x\\n", little_endian_read_16(packet, 0));
    h4_enforce_wake_on();
    if (h4_write_all(&packet_type, 1) || h4_write_all(packet, (size_t)size)) {
        perror("Bluetooth H4 write");
        if (trace) dockbridge_report(DB_EVENT_TRANSPORT_ERROR);
        return -1;
    }
    return 0;
}

""" + transport[end:]
    transport = transport.replace("    packet_handler(hci_packet[0], &hci_packet[1], read_pos-1);", """
    if (dockbridge_trace_hci_init() && hci_packet[0] == HCI_EVENT_PACKET && read_pos >= 7) {
        if (hci_packet[1] == HCI_EVENT_COMMAND_COMPLETE) {
            fprintf(stderr, "HCI init < 0x%04x complete status 0x%02x credits %u\\n",
                    little_endian_read_16(hci_packet, 4), hci_packet[6], hci_packet[3]);
        } else if (hci_packet[1] == HCI_EVENT_COMMAND_STATUS) {
            fprintf(stderr, "HCI init < 0x%04x status 0x%02x credits %u\\n",
                    little_endian_read_16(hci_packet, 5), hci_packet[3], hci_packet[4]);
        }
    }
    packet_handler(hci_packet[0], &hci_packet[1], read_pos-1);""")
    target = build_dir / original.name
    target.write_text(transport)
    return target


def patch_run_loop(btstack, build_dir):
    original = btstack / "platform/corefoundation/btstack_run_loop_corefoundation.m"
    run_loop = original.read_text()
    # Install the flag before adding the CFSocket and avoid dereferencing an absent socket.
    for action, operator in (("enable", "|= callback_types"), ("disable", "&= ~callback_types")):
        line = f"static void btstack_run_loop_embedded_{action}_data_source_callbacks(btstack_data_source_t * ds, uint16_t callback_types){{"
        run_loop = run_loop.replace(line, line + f"\n    ds->flags {operator};\n    if (!ds->item.next) return;")
    # CFAbsoluteTime is expressed in seconds. Transfer exactly one retained
    # reference through each timer and clear it before invoking BTstack.
    run_loop = run_loop.replace("    ts->process(ts);", """
    ts->item.next = NULL;
    CFRunLoopTimerInvalidate(timer);
    CFRelease(timer);
    ts->process(ts);""")
    run_loop = run_loop.replace("    CFAbsoluteTime fireDate = init_cf + ts->timeout;",
                                "    int32_t delay_ms = (int32_t)(ts->timeout - btstack_run_loop_corefoundation_get_time_ms());\n"
                                "    CFAbsoluteTime fireDate = CFAbsoluteTimeGetCurrent() + (delay_ms > 0 ? delay_ms / 1000.0 : 0);")
    run_loop = run_loop.replace("static void btstack_run_loop_corefoundation_add_timer(",
                                "static uint32_t btstack_run_loop_corefoundation_get_time_ms(void);\n\n"
                                "static void btstack_run_loop_corefoundation_add_timer(")
    run_loop = run_loop.replace("    CFRetain(timerRef);\n", "")
    run_loop = run_loop.replace("CFRelease((CFRunLoopTimerRef) ts->item.next);",
                                "CFRelease((CFRunLoopTimerRef) ts->item.next);\n        ts->item.next = NULL;")
    target = build_dir / original.name
    target.write_text(run_loop)
    return target


def patch_control(btstack, build_dir):
    original = btstack / "port/ios/src/btstack_control_iphone.m"
    control = '#include "bridge_ipc.h"\n' + original.read_text().replace('static char *os3xBlueTool = "BlueTool";',
                                           'static char *os3xBlueTool = "/usr/sbin/BlueTool";')
    control = control.replace('FILE * outputFile = popen(bluetool, "r+");',
                              'FILE * outputFile = popen(bluetool, "r+");\n'
                              '    if (!outputFile) { perror("BlueTool"); return -1; }')
    start = control.index("static void ioregistry_get_info(void){")
    end = control.index("uint32_t btstack_control_iphone_get_transport_speed", start)
    control = control[:start] + """
static int ioregistry_get_info(void) {
    mach_port_t master;
    if (IOMasterPort(MACH_PORT_NULL, &master) != KERN_SUCCESS) return -1;
    CFMutableDictionaryRef matching = IOServiceNameMatching("bluetooth");
    if (!matching) return -1;
    mach_port_t service = IOServiceGetMatchingService(master, matching);
    if (!service) return -1;
    CFTypeRef address = IORegistryEntrySearchCFProperty(service, "IODeviceTree",
                               CFSTR("local-mac-address"), kCFAllocatorDefault, 1);
    CFTypeRef speed = IORegistryEntrySearchCFProperty(service, "IODeviceTree",
                               CFSTR("transport-speed"), kCFAllocatorDefault, 1);
    int valid = address && CFGetTypeID(address) == CFDataGetTypeID() &&
                CFDataGetLength(address) == sizeof(local_mac_address) &&
                speed && CFGetTypeID(speed) == CFDataGetTypeID() &&
                CFDataGetLength(speed) == sizeof(transport_speed);
    if (valid) {
        CFDataGetBytes(address, CFRangeMake(0, sizeof(local_mac_address)), local_mac_address);
        CFDataGetBytes(speed, CFRangeMake(0, sizeof(transport_speed)), (uint8_t *)&transport_speed);
    }
    if (address) CFRelease(address);
    if (speed) CFRelease(speed);
    IOObjectRelease(service);
    return valid && transport_speed ? 0 : -1;
}

""" + control[end:]
    control = control.replace("ioregistry_get_info();",
                              'if (ioregistry_get_info()) { fprintf(stderr, "Bluetooth I/O registry unavailable\\n"); return -1; }')
    control = control.replace("static int iphone_on (void){",
                              "static int radio_detached;\n\nstatic int iphone_on (void){")
    control = control.replace('    log_info("iphone_on: unload BTServer\\n");',
                              '    dockbridge_report(DB_EVENT_RADIO_ACQUIRE);\n'
                              '    radio_detached = 1;\n    log_info("iphone_on: unload BTServer\\n");')
    control = control.replace("static int iphone_off(void){",
                              "static int iphone_off(void){\n"
                              "    if (!radio_detached) return 0;\n"
                              "    radio_detached = 0;")
    target = build_dir / original.name
    target.write_text(control)
    return target


def archive(entries):
    buffer = io.BytesIO()
    with tarfile.open(fileobj=buffer, mode="w:gz", format=tarfile.USTAR_FORMAT) as tar:
        for name, data, mode in entries:
            info = tarfile.TarInfo(name)
            info.mode = mode
            info.uname = "root"
            info.gname = "wheel"
            if data is None:
                info.type = tarfile.DIRTYPE
            else:
                info.size = len(data)
            tar.addfile(info, io.BytesIO(data) if data is not None else None)
    return buffer.getvalue()


def deb_package(binary, app, output):
    output.parent.mkdir(parents=True, exist_ok=True)
    control = (ROOT / "control").read_bytes()
    license_text = (ROOT / "BTstack-LICENSE.txt").read_bytes()
    sbc_license = (ROOT / "SBC-LICENSE.txt").read_bytes()
    scripts = [("./" + name, (ROOT / name).read_text().encode("utf-8"), 0o755)
               for name in ("postinst", "prerm", "postrm")]
    with output.open("wb") as deb:
        deb.write(b"!<arch>\n")
        for name, data in (
            ("debian-binary", b"2.0\n"),
            ("control.tar.gz", archive([("./control", control, 0o644), *scripts])),
            ("data.tar.gz", archive([
                ("./Applications", None, 0o755),
                ("./Applications/DockBridge.app", None, 0o755),
                ("./Applications/DockBridge.app/Info.plist", (ROOT / "AppInfo.plist").read_bytes(), 0o644),
                ("./Applications/DockBridge.app/DockBridge", app.read_bytes(), 0o755),
                ("./Applications/DockBridge.app/DockBridgeRounded.png",
                 (ROOT / "assets/DockBridgeRounded.png").read_bytes(), 0o644),
                ("./Applications/DockBridge.app/DockBridgeRounded@2x.png",
                 (ROOT / "assets/DockBridgeRounded@2x.png").read_bytes(), 0o644),
                ("./Library", None, 0o755),
                ("./Library/LaunchDaemons", None, 0o755),
                ("./Library/LaunchDaemons/local.dockbridge.service.plist",
                 (ROOT / "local.dockbridge.service.plist").read_bytes(), 0o644),
                ("./usr", None, 0o755),
                ("./usr/bin", None, 0o755),
                ("./usr/bin/dockbridge", binary.read_bytes(), 0o755),
                ("./var", None, 0o755),
                ("./var/lib", None, 0o755),
                ("./var/lib/dockbridge", None, 0o700),
                ("./var/lib/dockbridge-ipc", None, 0o755),
                ("./usr/share", None, 0o755),
                ("./usr/share/doc", None, 0o755),
                ("./usr/share/doc/local.dockbridge", None, 0o755),
                ("./usr/share/doc/local.dockbridge/copyright", license_text, 0o644),
                ("./usr/share/doc/local.dockbridge/SBC-LICENSE", sbc_license, 0o644),
            ])),
        ):
            deb.write(f"{name + '/':16}{0:<12}{0:<6}{0:<6}{'100644':8}{len(data):<10}`\n".encode("ascii"))
            deb.write(data)
            if len(data) % 2:
                deb.write(b"\n")


def verify_signature(binary):
    data = binary.read_bytes()
    magic, cpu, subtype, file_type, commands = struct.unpack_from("<5I", data)
    if (magic, cpu, subtype, file_type) != (0xfeedface, 12, 9, 2):
        raise ValueError("Expected a 32-bit ARMv7 Mach-O executable")
    offset = 28
    for _ in range(commands):
        command, size = struct.unpack_from("<II", data, offset)
        if command == 0x1d:
            start, length = struct.unpack_from("<II", data, offset + 8)
            signature = data[start:start + length]
            magic, _, count = struct.unpack_from(">III", signature)
            if magic != 0xfade0cc0:
                break
            for slot, position in struct.iter_unpack(">II", signature[12:12 + 8 * count]):
                if slot == 0 and signature[position + 37] == 1:
                    return
            break
        offset += size
    raise ValueError("A primary SHA-1 code signature is required by the iOS 6 build")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for arg in ("btstack", "sdk", "clang", "ld", "ldid", "output"):
        parser.add_argument("--" + arg, required=True, type=Path)
    parser.add_argument("--codesign-allocate", type=Path)
    args = parser.parse_args()
    btstack = args.btstack.resolve()
    rev = run("git", "-C", btstack, "rev-parse", "HEAD", capture_output=True, text=True).stdout.strip()
    if rev != BTSTACK_REV:
        raise SystemExit("Unexpected BTstack version: " + rev)
    build = ROOT / "build"
    build.mkdir(exist_ok=True)
    sdk = toolpath(args.sdk)
    src = btstack / "src"
    ios = btstack / "port/ios/src"
    posix = btstack / "platform/posix"
    cf = btstack / "platform/corefoundation"
    sbc = btstack / "3rd-party/bluedroid/decoder"
    sources = [ROOT / "receiver.c", ROOT / "audio_queue.c", ROOT / "service.c", ROOT / "media_info.c",
               patch_demo(btstack, build), patch_avrcp_controller(btstack, build),
               patch_hci(btstack, build), patch_transport(btstack, build), patch_control(btstack, build),
               patch_run_loop(btstack, build), posix / "btstack_tlv_posix.c"]
    sources += [src / (name + ".c") for name in """
        ad_parser btstack_audio btstack_linked_list btstack_memory btstack_memory_pool
        btstack_ring_buffer btstack_resample btstack_run_loop btstack_tlv btstack_util
        hci_cmd hci_dump l2cap l2cap_signaling
        classic/a2dp_sink classic/avdtp classic/avdtp_acceptor
        classic/avdtp_initiator classic/avdtp_sink classic/avdtp_source classic/avdtp_util
        classic/avrcp classic/avrcp_target
        classic/btstack_link_key_db_tlv classic/btstack_sbc_decoder_bluedroid
        classic/btstack_sbc_plc classic/device_id_server classic/sdp_client
        classic/sdp_server classic/sdp_util
    """.split()]
    codec_files = re.findall(r"([\w-]+\.c)", (sbc / "Makefile.inc").read_text())
    sources += [sbc / "srce" / name for name in codec_files]
    includes = [ROOT, src, ios, posix, cf, sbc / "include"]
    flags = ["-target", "armv7-apple-ios6.0", "-miphoneos-version-min=6.0", "-isysroot", sdk,
             "-O2", "-fno-stack-protector",
             "-ffunction-sections", "-fdata-sections", "-Wno-deprecated-declarations",
             *("-I" + toolpath(directory) for directory in includes)]

    def compile_source(index_and_source):
        index, source = index_and_source
        obj = build / f"{index:02d}-{source.stem}.o"
        extra = ["-fblocks"] if source.name == "DockBridgeApp.m" else []
        # Keep BTstack's source names in logs without embedding the build machine's paths.
        result = subprocess.run([str(args.clang.resolve()), *flags, *extra, "-c", source.name,
                                 "-o", toolpath(obj)], cwd=source.parent, capture_output=True, text=True)
        return obj, result

    with ThreadPoolExecutor(max_workers=6) as pool:
        results = list(pool.map(compile_source, enumerate(sources)))
    errors = [result.stdout + result.stderr for _, result in results if result.returncode]
    if errors:
        raise SystemExit("\n".join(errors))
    objects = [obj for obj, _ in results]
    app_object, app_result = compile_source((len(sources), ROOT / "DockBridgeApp.m"))
    if app_result.returncode:
        raise SystemExit(app_result.stdout + app_result.stderr)
    binary = build / "dockbridge"
    app = build / "dockbridge-ui"
    frameworks = args.sdk.resolve() / "System/Library/Frameworks"
    libraries = args.sdk.resolve() / "usr/lib"
    def framework_path(name):
        folder = frameworks / (name + ".framework")
        if not folder.exists():
            folder = frameworks.parent / "PrivateFrameworks" / (name + ".framework")
        candidates = [folder / name, folder / (name + ".tbd"), folder / "Versions/A" / name]
        return toolpath(next(path for path in candidates if path.is_file()))
    system_args = []
    for name in ("libSystem.B", "libobjc.A"):
        system_args.append(toolpath(next(path for ext in (".dylib", ".tbd")
                                        if (path := libraries / (name + ext)).is_file())))
    env = os.environ.copy()
    if args.codesign_allocate:
        env["CODESIGN_ALLOCATE"] = toolpath(args.codesign_allocate)

    def link_and_sign(target, inputs, framework_names):
        result = subprocess.run([str(args.ld.resolve()), "-arch", "armv7", "-iphoneos_version_min",
                                 "6.0", "-syslibroot", sdk, "-dead_strip", "-o", toolpath(target),
                                 *(toolpath(obj) for obj in inputs),
                                 *(framework_path(name) for name in framework_names), *system_args],
                                capture_output=True, text=True)
        if result.returncode:
            raise SystemExit(result.stdout + result.stderr)
        run(args.ldid.resolve(), "-S", toolpath(target), env=env)
        verify_signature(target)

    link_and_sign(binary, objects, ("Foundation", "CoreFoundation", "AudioToolbox", "IOKit"))
    link_and_sign(app, [app_object], ("UIKit", "Foundation", "CoreFoundation", "CoreGraphics", "QuartzCore"))
    license_file = btstack / "LICENSE"
    if not license_file.exists():
        license_file = btstack / "LICENSE.txt"
    shutil.copyfile(license_file, ROOT / "BTstack-LICENSE.txt")
    deb_package(binary, app, args.output.resolve())


if __name__ == "__main__":
    main()
