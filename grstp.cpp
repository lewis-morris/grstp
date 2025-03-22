#include <gst/gst.h>
#include <iostream>
#include <string>
#include <iomanip>
#include <sstream>
#include <cctype>

// Simple URL-encoder for the RTSP credentials
std::string url_encode(const std::string& value) {
    std::ostringstream escaped;
    escaped.fill('0');
    escaped << std::hex;

    for (unsigned char c : value) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            escaped << c;
        } else {
            escaped << '%' << std::setw(2) << int(c);
        }
    }
    return escaped.str();
}

// Struct for command-line arguments
struct Args {
    std::string camIp    = "192.168.0.10";
    int         camPort  = 554;
    std::string user     = "admin";
    std::string pass     = "password";
    std::string rtspPath = "h264Preview_01_sub";

    // Where to stream out
    std::string outIp  = "127.0.0.1";
    int         outPort= 23445;
    bool        useUdp = false;
};

// Parse command-line arguments
Args parse_args(int argc, char** argv) {
    Args args;

    auto print_help = []() {
        std::cout << "Usage: grstp [options]\n\n"
                  << "Options:\n"
                  << "  --cam-ip <ip>         Camera IP (default: 192.168.0.10)\n"
                  << "  --cam-port <port>     Camera RTSP port (default: 554)\n"
                  << "  --username <user>     RTSP username (default: admin)\n"
                  << "  --password <pass>     RTSP password (default: password)\n"
                  << "  --rtsp-path <path>    RTSP path (default: h264Preview_01_sub)\n"
                  << "  --out-ip <ip>         Output IP (default: 127.0.0.1)\n"
                  << "  --out-port <port>     Output port (default: 23445)\n"
                  << "  --udp                 Use UDP instead of TCP\n"
                  << "  -h, --help            Print help\n";
    };

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--cam-ip" && i+1 < argc) {
            args.camIp = argv[++i];
        } else if (a == "--cam-port" && i+1 < argc) {
            args.camPort = std::stoi(argv[++i]);
        } else if (a == "--username" && i+1 < argc) {
            args.user = argv[++i];
        } else if (a == "--password" && i+1 < argc) {
            args.pass = argv[++i];
        } else if (a == "--rtsp-path" && i+1 < argc) {
            args.rtspPath = argv[++i];
        } else if (a == "--out-ip" && i+1 < argc) {
            args.outIp = argv[++i];
        } else if (a == "--out-port" && i+1 < argc) {
            args.outPort = std::stoi(argv[++i]);
        } else if (a == "--udp") {
            args.useUdp = true;
        } else if (a == "--help" || a == "-h") {
            print_help();
            exit(0);
        } else {
            std::cerr << "Unknown arg: " << a << "\n";
            print_help();
            exit(1);
        }
    }
    return args;
}

// Build the RTSP URL
std::string make_rtsp_url(const Args& args) {
    return "rtsp://" + url_encode(args.user) + ":" + url_encode(args.pass) +
           "@" + args.camIp + ":" + std::to_string(args.camPort) +
           "/" + args.rtspPath;
}

int main(int argc, char** argv) {
    // 1. Parse command-line arguments
    Args args = parse_args(argc, argv);

    // 2. Initialize GStreamer
    gst_init(&argc, &argv);

    // 3. Build the pipeline description
    // We'll do a single flow (no appsink, no tee):
    //
    //   rtspsrc location=URL latency=0 !
    //     queue max-size-buffers=1 leaky=downstream !
    //     rtph264depay ! h264parse ! avdec_h264 !
    //     videoconvert ! videoscale !
    //     video/x-raw,format=RGB16,width=320,height=240 !
    //     queue max-size-buffers=1 leaky=downstream !
    //     <sink>
    //
    // <sink> is either udpsink or tcpserversink based on args.useUdp

    std::string rtspUrl = make_rtsp_url(args);

    std::string sinkBlock;
    if (args.useUdp) {
        // Example: "udpsink host=127.0.0.1 port=23445 sync=false"
        sinkBlock = "udpsink host=" + args.outIp +
                    " port=" + std::to_string(args.outPort) +
                    " sync=false";
    } else {
        // Example: "tcpserversink host=127.0.0.1 port=23445 sync=false"
        sinkBlock = "tcpserversink host=" + args.outIp +
                    " port=" + std::to_string(args.outPort) +
                    " sync=false";
    }

    std::string pipelineDesc =
        "rtspsrc location=" + rtspUrl + " latency=0 ! "
        "queue max-size-buffers=1 leaky=downstream ! "
        "rtph264depay ! h264parse ! avdec_h264 ! "
        "videoconvert ! videoscale ! "
        "video/x-raw,format=RGB16,width=320,height=240 ! "
        "queue max-size-buffers=1 leaky=downstream ! " +
        sinkBlock;

    std::cout << "Pipeline:\n" << pipelineDesc << "\n";

    // 4. Create pipeline
    GError* error = nullptr;
    GstElement* pipeline = gst_parse_launch(pipelineDesc.c_str(), &error);
    if (!pipeline || error) {
        std::cerr << "Failed to create pipeline.\n";
        if (error) {
            std::cerr << error->message << "\n";
            g_error_free(error);
        }
        return 1;
    }

    // 5. Set pipeline to PLAYING
    gst_element_set_state(pipeline, GST_STATE_PLAYING);

    // 6. Listen for errors and EOS
    GstBus* bus = gst_element_get_bus(pipeline);
    bool done = false;
    while (!done) {
        GstMessage* msg = gst_bus_timed_pop_filtered(
            bus, GST_CLOCK_TIME_NONE,
            (GstMessageType)(GST_MESSAGE_ERROR | GST_MESSAGE_EOS | GST_MESSAGE_STATE_CHANGED));

        if (!msg) {
            // Should not happen with GST_CLOCK_TIME_NONE
            continue;
        }
        switch (GST_MESSAGE_TYPE(msg)) {
            case GST_MESSAGE_ERROR: {
                GError* err;
                gchar* debug;
                gst_message_parse_error(msg, &err, &debug);
                std::cerr << "[Error] " << err->message << "\n";
                g_error_free(err);
                g_free(debug);
                done = true;
                break;
            }
            case GST_MESSAGE_EOS: {
                std::cout << "[EOS] End of Stream\n";
                done = true;
                break;
            }
            case GST_MESSAGE_STATE_CHANGED: {
                if (GST_MESSAGE_SRC(msg) == GST_OBJECT(pipeline)) {
                    GstState oldState, newState, pending;
                    gst_message_parse_state_changed(msg, &oldState, &newState, &pending);
                    std::cout << "Pipeline state changed from "
                              << gst_element_state_get_name(oldState) << " to "
                              << gst_element_state_get_name(newState) << "\n";
                }
                break;
            }
            default:
                // Not handling other message types
                break;
        }
        gst_message_unref(msg);
    }

    // 7. Cleanup
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(bus);
    gst_object_unref(pipeline);

    std::cout << "Exiting cleanly.\n";
    return 0;
}
