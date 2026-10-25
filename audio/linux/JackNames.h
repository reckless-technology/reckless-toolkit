#pragma once

// Every JACK function the toolkit calls - its JACK back end (JackDevice.cpp) and the device suite's rig - under the
// name of the run-time loader's function that stands in for it (JackLoader.cpp). Each file that calls JACK includes
// this header before jack/jack.h, so its calls - and the declarations jack/jack.h makes of them - name the loader's
// functions, and nothing in the program asks the dynamic linker for libjack: a machine without it starts as any other
// and lists no JACK device, and one with it - PipeWire's JACK, or a JACK server's own - plays through it. A function
// the toolkit starts calling must be added here and in the loader. The link names any missed one as undefined - but not
// one jack/jack.h declares weak (jack_port_get_latency_range among them), which a missed rename would leave null: so
// the loader is the only place a JACK symbol is looked up, and a call is never made but through a name defined here.

/// libjack's `jack_activate`, under the loader's name.
#define jack_activate reckless_jack_activate
/// libjack's `jack_client_close`, under the loader's name.
#define jack_client_close reckless_jack_client_close
/// libjack's `jack_client_open`, under the loader's name.
#define jack_client_open reckless_jack_client_open
/// libjack's `jack_connect`, under the loader's name.
#define jack_connect reckless_jack_connect
/// libjack's `jack_deactivate`, under the loader's name.
#define jack_deactivate reckless_jack_deactivate
/// libjack's `jack_free`, under the loader's name.
#define jack_free reckless_jack_free
/// libjack's `jack_get_buffer_size`, under the loader's name.
#define jack_get_buffer_size reckless_jack_get_buffer_size
/// libjack's `jack_get_ports`, under the loader's name.
#define jack_get_ports reckless_jack_get_ports
/// libjack's `jack_get_sample_rate`, under the loader's name.
#define jack_get_sample_rate reckless_jack_get_sample_rate
/// libjack's `jack_on_shutdown`, under the loader's name.
#define jack_on_shutdown reckless_jack_on_shutdown
/// libjack's `jack_port_by_name`, under the loader's name.
#define jack_port_by_name reckless_jack_port_by_name
/// libjack's `jack_port_get_buffer`, under the loader's name.
#define jack_port_get_buffer reckless_jack_port_get_buffer
/// libjack's `jack_port_get_latency_range`, under the loader's name.
#define jack_port_get_latency_range reckless_jack_port_get_latency_range
/// libjack's `jack_port_name`, under the loader's name.
#define jack_port_name reckless_jack_port_name
/// libjack's `jack_port_register`, under the loader's name.
#define jack_port_register reckless_jack_port_register
/// libjack's `jack_port_unregister`, under the loader's name.
#define jack_port_unregister reckless_jack_port_unregister
/// libjack's `jack_set_buffer_size`, under the loader's name.
#define jack_set_buffer_size reckless_jack_set_buffer_size
/// libjack's `jack_set_buffer_size_callback`, under the loader's name.
#define jack_set_buffer_size_callback reckless_jack_set_buffer_size_callback
/// libjack's `jack_set_error_function`, under the loader's name.
#define jack_set_error_function reckless_jack_set_error_function
/// libjack's `jack_set_process_callback`, under the loader's name.
#define jack_set_process_callback reckless_jack_set_process_callback
/// libjack's `jack_set_xrun_callback`, under the loader's name.
#define jack_set_xrun_callback reckless_jack_set_xrun_callback

/** The loader's own answer, no libjack function: whether the libjack it loaded is PipeWire's (JackLoader.cpp).
    @return  1 for PipeWire's libjack; 0 for JACK's own, or none */
extern "C" int reckless_jack_is_pipewire(void);
