#include <gst/gst.h>
#include <gst/app/app.h>
#include <iostream>
#include <string>
#include <thread>
#include <atomic>
#include <iomanip>
#include <sstream>
#include <cctype>

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
// Atomic flag to signal the appsink thread to stop
static std::atomic<bool> gQuit(false);

// Simple function to parse command-line arguments
// (Very minimal error-checking for brevity.)
struct Args {
    std::string camIp       = "192.168.0.10";
    int         camPort     = 554;
    std::string user        = "admin";
    std::string pass        = "password";
    std::string rtspPath    = "h264Preview_01_sub";
    std::string outIp       = "127.0.0.1";
    int         outPort     = 23445;
    bool        useUdp      = false;
};

Args parse_args(int argc, char** argv) {
    Args args;

    auto print_help = []() {
        std::cout << "Usage: grstp [options]\n"
                  << "\nOptions:\n"
                  << "  --cam-ip <ip>         Camera IP address (default: 192.168.0.10)\n"
                  << "  --cam-port <port>     Camera RTSP port (default: 554)\n"
                  << "  --username <user>     RTSP username (default: admin)\n"
                  << "  --password <pass>     RTSP password (default: password)\n"
                  << "  --rtsp-path <path>    RTSP stream path (default: h264Preview_01_sub)\n"
                  << "  --out-ip <ip>         Output host IP (default: 192.168.0.2)\n"
                  << "  --out-port <port>     Output port (default: 23445)\n"
                  << "  --udp                 Use UDP instead of TCP\n"
                  << "  -h, --help            Show this help message\n"
                  << std::endl;
    };

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--cam-ip" && i + 1 < argc) {
            args.camIp = argv[++i];
        } else if (a == "--cam-port" && i + 1 < argc) {
            args.camPort = std::stoi(argv[++i]);
        } else if (a == "--username" && i + 1 < argc) {
            args.user = argv[++i];
        } else if (a == "--password" && i + 1 < argc) {
            args.pass = argv[++i];
        } else if (a == "--rtsp-path" && i + 1 < argc) {
            args.rtspPath = argv[++i];
        } else if (a == "--out-ip" && i + 1 < argc) {
            args.outIp = argv[++i];
        } else if (a == "--out-port" && i + 1 < argc) {
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
    return "rtsp://" + url_encode(args.user) + ":" + url_encode(args.pass) + "@" +
           args.camIp + ":" + std::to_string(args.camPort) + "/" +
           args.rtspPath;
}

// Appsink thread: continuously pull+discard frames so queue stays empty
void appsink_thread(GstElement* appsink) {
    while (!gQuit.load()) {
        // Blocking call: wait for next frame
        GstSample* sample = gst_app_sink_pull_sample(GST_APP_SINK(appsink));
        if (!sample) {
            // Null means EOS or error
            break;
        }
        // If you needed to process the frame, you'd do it here.
        // Right now we just discard.
        gst_sample_unref(sample);
    }
    std::cerr << "[appsink_thread] Exiting\n";
}

int main(int argc, char** argv) {
    // 1) Parse Args
    Args args = parse_args(argc, argv);

    // 2) Initialize GStreamer
    gst_init(&argc, &argv);

    // 3) Construct pipeline string
    // Basic pipeline concept:
    //    rtspsrc location=RTSP_URL latency=0 !
    //       rtph264depay ! h264parse ! avdec_h264 ! videoconvert ! videoscale !
    //       queue max-size-buffers=1 leaky=downstream !
    //       video/x-raw,format=RGB16,width=320,height=240 !
    //       tee name=t
    //         t. ! queue max-size-buffers=1 leaky=downstream ! <sink choice>
    //         t. ! queue max-size-buffers=1 leaky=downstream ! appsink name=mysink
    //
    // <sink choice> is either tcpserversink or udpsink depending on --udp.

    std::string rtspUrl   = make_rtsp_url(args);
    std::string sinkBlock;

    if (args.useUdp) {
        // e.g. "udpsink host=192.168.0.2 port=23445"
        sinkBlock = "udpsink host=" + args.outIp + " port=" + std::to_string(args.outPort) + " sync=false ";
    } else {
        // e.g. "tcpserversink host=192.168.0.2 port=23445"
        sinkBlock = "tcpserversink host=" + args.outIp + " port=" + std::to_string(args.outPort) + " sync=false ";
    }

    std::string pipelineDesc =
        "rtspsrc location=" + rtspUrl + " latency=0 ! "
        "rtph264depay ! h264parse ! avdec_h264 ! "
        "videoconvert ! videoscale ! "
        // This queue ensures we only keep 1 buffer at a time before tee
        "queue max-size-buffers=1 leaky=downstream ! "
        "video/x-raw,format=RGB16,width=320,height=240 ! "
        "tee name=t "
        // Branch 1: to output sink
        "t. ! queue max-size-buffers=1 leaky=downstream ! " + sinkBlock +
        // Branch 2: to appsink
        "t. ! queue max-size-buffers=1 leaky=downstream ! appsink name=mysink sync=false emit-signals=false";

    std::cout << "Pipeline:\n" << pipelineDesc << "\n";

    // 4) Create pipeline from the description
    GError* error = nullptr;
    GstElement* pipeline = gst_parse_launch(pipelineDesc.c_str(), &error);
    if (!pipeline || error) {
        std::cerr << "Failed to create pipeline:\n"
                  << (error ? error->message : "(unknown error)") << "\n";
        if (error) g_error_free(error);
        return 1;
    }

    // 5) Retrieve the appsink
    GstElement* appsink = gst_bin_get_by_name(GST_BIN(pipeline), "mysink");
    if (!appsink) {
        std::cerr << "Could not find appsink named 'mysink' in the pipeline.\n";
        gst_object_unref(pipeline);
        return 1;
    }
    // Optionally set drop=TRUE, max-buffers=1 in the appsink itself
    // to ensure no extra frames queue up if we read slowly:
    //   gst_app_sink_set_drop(GST_APP_SINK(appsink), TRUE);
    //   gst_app_sink_set_max_buffers(GST_APP_SINK(appsink), 1);

    // 6) Start playing
    gst_element_set_state(pipeline, GST_STATE_PLAYING);

    // 7) Start the appsink thread
    std::thread t(appsink_thread, appsink);

    // 8) Wait for errors/EOS on the bus
    GstBus* bus = gst_element_get_bus(pipeline);
    bool done   = false;
    while (!done) {
        GstMessage* msg = gst_bus_timed_pop_filtered(
            bus, GST_CLOCK_TIME_NONE,
            (GstMessageType)(GST_MESSAGE_ERROR | GST_MESSAGE_EOS | GST_MESSAGE_STATE_CHANGED));

        if (!msg) {
            // Should never happen with GST_CLOCK_TIME_NONE, but just in case
            continue;
        }

        switch (GST_MESSAGE_TYPE(msg)) {
        case GST_MESSAGE_ERROR: {
            GError* err;
            gchar*  debug;
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
            // Print pipeline state changes
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
            break;
        }

        gst_message_unref(msg);
    }

    // 9) Clean up
    gQuit.store(true);
    // Let the appsink know we’re done so pull_sample() returns NULL

    if (t.joinable()) {
        t.join();
    }

    gst_object_unref(appsink);

    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
    gst_object_unref(bus);

    std::cout << "Exiting cleanly.\n";
    return 0;
}
