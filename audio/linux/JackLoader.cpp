// libjack, loaded by name the first time anything asks of JACK: nothing in the program is linked to it, so a machine
// without it starts as any other and runs on ALSA (the root CMakeLists.txt says why). Each function the toolkit calls -
// JackNames.h gives them this file's names - is looked up in the library once and called through; with no library, or
// one without the function, each answers as libjack does when it cannot serve: no client, no ports, a failure code,
// nothing done. The JACK back end (JackDevice.cpp) then finds no JACK server and lists no JACK device.
//
// The library is opened by its versioned name, libjack.so.0 - the run-time package's, JACK's own or PipeWire's - and
// by its unversioned one after, which only a development package installs. It stays open for the process's life, as
// a client and the server's threads into it may outlive any one stream.
#include "JackNames.h"

#include <dlfcn.h>
#include <jack/jack.h>

namespace
{
/** libjack's functions as the toolkit calls them, each null where the library or the symbol is missing. */
struct Jack
{
    decltype(&reckless_jack_activate)               activate               = nullptr; ///< `jack_activate`
    decltype(&reckless_jack_client_close)           client_close           = nullptr; ///< `jack_client_close`
    decltype(&reckless_jack_client_open)            client_open            = nullptr; ///< `jack_client_open`
    decltype(&reckless_jack_connect)                connect                = nullptr; ///< `jack_connect`
    decltype(&reckless_jack_deactivate)             deactivate             = nullptr; ///< `jack_deactivate`
    decltype(&reckless_jack_free)                   free                   = nullptr; ///< `jack_free`
    decltype(&reckless_jack_get_buffer_size)        get_buffer_size        = nullptr; ///< `jack_get_buffer_size`
    decltype(&reckless_jack_get_ports)              get_ports              = nullptr; ///< `jack_get_ports`
    decltype(&reckless_jack_get_sample_rate)        get_sample_rate        = nullptr; ///< `jack_get_sample_rate`
    decltype(&reckless_jack_on_shutdown)            on_shutdown            = nullptr; ///< `jack_on_shutdown`
    decltype(&reckless_jack_port_by_name)           port_by_name           = nullptr; ///< `jack_port_by_name`
    decltype(&reckless_jack_port_get_buffer)        port_get_buffer        = nullptr; ///< `jack_port_get_buffer`
    decltype(&reckless_jack_port_get_latency_range) port_get_latency_range = nullptr; ///< `jack_port_get_latency_range`
    decltype(&reckless_jack_port_name)              port_name              = nullptr; ///< `jack_port_name`
    decltype(&reckless_jack_port_register)          port_register          = nullptr; ///< `jack_port_register`
    decltype(&reckless_jack_port_unregister)        port_unregister        = nullptr; ///< `jack_port_unregister`
    decltype(&reckless_jack_set_error_function)     set_error_function     = nullptr; ///< `jack_set_error_function`
    decltype(&reckless_jack_set_buffer_size)        set_buffer_size        = nullptr; ///< `jack_set_buffer_size`
    decltype(&reckless_jack_set_buffer_size_callback) set_buffer_size_callback =
        nullptr;                                                                  ///< `jack_set_buffer_size_callback`
    decltype(&reckless_jack_set_process_callback) set_process_callback = nullptr; ///< `jack_set_process_callback`
    decltype(&reckless_jack_set_xrun_callback)    set_xrun_callback    = nullptr; ///< `jack_set_xrun_callback`
    bool is_pipewire = false; ///< whether the library is PipeWire's, which brings libpipewire in with it
};

/** `function` from `library` by its libjack name, or null. A symbol's address as a function pointer is what dlsym
    exists to hand out (POSIX requires the conversion to work).
    @tparam Function  the function pointer's type
    @param library    the dlopen handle, or null for none
    @param name       the symbol's name in libjack
    @param function   set to the symbol's address, or null */
template <typename Function> void Find(void *library, const char *name, Function &function)
{
    function = library != nullptr ? reinterpret_cast<Function>(dlsym(library, name)) : nullptr;
}

/** libjack opened by its versioned name and else its unversioned one, and each function the toolkit calls looked up
    in it.
    @return  the functions, each null where the library or the symbol is missing */
Jack Load()
{
    void *library = dlopen("libjack.so.0", RTLD_NOW | RTLD_LOCAL);
    if (library == nullptr)
    {
        library = dlopen("libjack.so", RTLD_NOW | RTLD_LOCAL);
    }
    Jack jack;
    Find(library, "jack_activate", jack.activate);
    Find(library, "jack_client_close", jack.client_close);
    Find(library, "jack_client_open", jack.client_open);
    Find(library, "jack_connect", jack.connect);
    Find(library, "jack_deactivate", jack.deactivate);
    Find(library, "jack_free", jack.free);
    Find(library, "jack_get_buffer_size", jack.get_buffer_size);
    Find(library, "jack_get_ports", jack.get_ports);
    Find(library, "jack_get_sample_rate", jack.get_sample_rate);
    Find(library, "jack_on_shutdown", jack.on_shutdown);
    Find(library, "jack_port_by_name", jack.port_by_name);
    Find(library, "jack_port_get_buffer", jack.port_get_buffer);
    Find(library, "jack_port_get_latency_range", jack.port_get_latency_range);
    Find(library, "jack_port_name", jack.port_name);
    Find(library, "jack_port_register", jack.port_register);
    Find(library, "jack_port_unregister", jack.port_unregister);
    Find(library, "jack_set_error_function", jack.set_error_function);
    Find(library, "jack_set_buffer_size", jack.set_buffer_size);
    Find(library, "jack_set_buffer_size_callback", jack.set_buffer_size_callback);
    Find(library, "jack_set_process_callback", jack.set_process_callback);
    Find(library, "jack_set_xrun_callback", jack.set_xrun_callback);
    // PipeWire's libjack reports JACK's own version, so it is known by what it links: dlsym through a handle searches
    // the library's dependencies too, and only PipeWire's brings libpipewire, and pw_init, with it.
    jack.is_pipewire = library != nullptr && dlsym(library, "pw_init") != nullptr;
    return jack;
}

/** The library, opened on the first call from any thread (a function-local static is initialised once).
    @return  the functions, loaded once */
const Jack &Library()
{
    static const Jack jack = Load();
    return jack;
}
} // namespace

extern "C"
{
    /** libjack's `jack_activate`: the client's process callback starts to run.
        @param client  the client
        @return  0 on success; -1 with no library or on failure */
    int reckless_jack_activate(jack_client_t *client)
    {
        const auto function = Library().activate;
        return function != nullptr ? function(client) : -1;
    }

    /** libjack's `jack_client_close`: the client disconnected from the server and freed.
        @param client  the client
        @return  0 on success; -1 with no library or on failure */
    int reckless_jack_client_close(jack_client_t *client)
    {
        const auto function = Library().client_close;
        return function != nullptr ? function(client) : -1;
    }

    /** The server's name, which a client may pass after `status` with JackServerName, is not handed on: RtAudio opens
       the default server and passes none. With no library, the status says the server could not be reached.
        @param client_name  the client's name
        @param options      the JackOptions
        @param status       where the JackStatus is written; may be null
        @return             the client; null with no library or on failure */
    jack_client_t *reckless_jack_client_open(const char *client_name, jack_options_t options, jack_status_t *status,
                                             ...)
    {
        const auto function = Library().client_open;
        if (function == nullptr)
        {
            if (status != nullptr)
            {
                *status = static_cast<jack_status_t>(JackFailure | JackServerFailed);
            }
            return nullptr;
        }
        return function(client_name, options, status);
    }

    /** libjack's `jack_connect`: two ports connected.
        @param client  the client
        @param source_port  the full name of the output port
        @param destination_port  the full name of the input port
        @return  0 on success; -1 with no library or on failure */
    int reckless_jack_connect(jack_client_t *client, const char *source_port, const char *destination_port)
    {
        const auto function = Library().connect;
        return function != nullptr ? function(client, source_port, destination_port) : -1;
    }

    /** libjack's `jack_deactivate`: the client's process callback stops running.
        @param client  the client
        @return  0 on success; -1 with no library or on failure */
    int reckless_jack_deactivate(jack_client_t *client)
    {
        const auto function = Library().deactivate;
        return function != nullptr ? function(client) : -1;
    }

    /** libjack's `jack_free`: memory libjack handed out - a port list - given back to the allocator that made it.
        Nothing with no library, which can have handed out nothing.
        @param pointer  what libjack returned; may be null */
    void reckless_jack_free(void *pointer)
    {
        if (const auto function = Library().free)
        {
            function(pointer);
        }
    }

    /** libjack's `jack_get_buffer_size`: the server's block.
        @param client  the client
        @return  the block, in frames; 0 with no library */
    jack_nframes_t reckless_jack_get_buffer_size(jack_client_t *client)
    {
        const auto function = Library().get_buffer_size;
        return function != nullptr ? function(client) : 0;
    }

    /** libjack's `jack_get_ports`: the ports whose names, types and flags match.
        @param client  the client
        @param port_name_pattern  a regular expression the port's name must match, or null for any
        @param type_name_pattern  a regular expression the port's type must match, or null for any
        @param flags  the JackPortFlags the port must carry, or 0 for any
        @return  a null-terminated array of port names, to be freed with jack_free; null with no library or no match */
    const char **reckless_jack_get_ports(jack_client_t *client, const char *port_name_pattern,
                                         const char *type_name_pattern, unsigned long flags)
    {
        const auto function = Library().get_ports;
        return function != nullptr ? function(client, port_name_pattern, type_name_pattern, flags) : nullptr;
    }

    /** libjack's `jack_get_sample_rate`: the server's rate.
        @param client  the client
        @return  the rate, in hertz; 0 with no library */
    jack_nframes_t reckless_jack_get_sample_rate(jack_client_t *client)
    {
        const auto function = Library().get_sample_rate;
        return function != nullptr ? function(client) : 0;
    }

    /** libjack's `jack_on_shutdown`: a callback for the server shutting the client down. Nothing with no library.
        @param client  the client
        @param shutdown_callback  called, on a thread of the server's, when the server shuts the client down
        @param arg  passed to `shutdown_callback` */
    void reckless_jack_on_shutdown(jack_client_t *client, JackShutdownCallback shutdown_callback, void *arg)
    {
        if (const auto function = Library().on_shutdown)
        {
            function(client, shutdown_callback, arg);
        }
    }

    /** libjack's `jack_port_by_name`: the port of a full name.
        @param client  the client
        @param port_name  the port's full name
        @return  the port; null with no library or no such port */
    jack_port_t *reckless_jack_port_by_name(jack_client_t *client, const char *port_name)
    {
        const auto function = Library().port_by_name;
        return function != nullptr ? function(client, port_name) : nullptr;
    }

    /** libjack's `jack_port_get_buffer`: a port's buffer for this cycle. Audio thread.
        @param port  the port
        @param frames  the frames in the cycle
        @return  the buffer; null with no library */
    void *reckless_jack_port_get_buffer(jack_port_t *port, jack_nframes_t frames)
    {
        const auto function = Library().port_get_buffer;
        return function != nullptr ? function(port, frames) : nullptr;
    }

    /** A library too old to report a port's latency range reports none: the range stays as the caller set it.
        @param port   the port
        @param mode   which latency, capture or playback
        @param range  where the range is written */
    void reckless_jack_port_get_latency_range(jack_port_t *port, jack_latency_callback_mode_t mode,
                                              jack_latency_range_t *range)
    {
        if (const auto function = Library().port_get_latency_range)
        {
            function(port, mode, range);
        }
    }

    /** libjack's `jack_port_name`: a port's full name.
        @param port  the port
        @return  the name, owned by the library; null with no library */
    const char *reckless_jack_port_name(const jack_port_t *port)
    {
        const auto function = Library().port_name;
        return function != nullptr ? function(port) : nullptr;
    }

    /** libjack's `jack_port_register`: a port made for the client.
        @param client  the client
        @param port_name  the port's short name
        @param port_type  the port's type, the default audio type for audio
        @param flags  the JackPortFlags: direction and so on
        @param buffer_size  the buffer's size for a type that needs one, 0 for audio
        @return  the port; null with no library or on failure */
    jack_port_t *reckless_jack_port_register(jack_client_t *client, const char *port_name, const char *port_type,
                                             unsigned long flags, unsigned long buffer_size)
    {
        const auto function = Library().port_register;
        return function != nullptr ? function(client, port_name, port_type, flags, buffer_size) : nullptr;
    }

    /** libjack's `jack_port_unregister`: a port removed from the client.
        @param client  the client
        @param port  the port
        @return  0 on success; -1 with no library or on failure */
    int reckless_jack_port_unregister(jack_client_t *client, jack_port_t *port)
    {
        const auto function = Library().port_unregister;
        return function != nullptr ? function(client, port) : -1;
    }

    /** libjack's `jack_set_error_function`: where the library's error text goes. Nothing with no library.
        @param report  called with each error's text */
    void reckless_jack_set_error_function(void (*report)(const char *))
    {
        if (const auto function = Library().set_error_function)
        {
            function(report);
        }
    }

    /** libjack's `jack_set_process_callback`: the function that runs each cycle.
        @param client  the client
        @param process_callback  called each cycle on the server's audio thread
        @param arg  passed to `process_callback`
        @return  0 on success; -1 with no library or on failure */
    int reckless_jack_set_process_callback(jack_client_t *client, JackProcessCallback process_callback, void *arg)
    {
        const auto function = Library().set_process_callback;
        return function != nullptr ? function(client, process_callback, arg) : -1;
    }

    /** libjack's `jack_set_buffer_size`: the server's block changed, for every client - jackd's own; PipeWire's sets
        a quantum forced on its whole graph, which outlives the client (JackDevice.cpp asks it of jackd alone).
        @param client  the client
        @param frames  the block, in frames
        @return  0 on success; -1 with no library or on failure */
    int reckless_jack_set_buffer_size(jack_client_t *client, jack_nframes_t frames)
    {
        const auto function = Library().set_buffer_size;
        return function != nullptr ? function(client, frames) : -1;
    }

    /** libjack's `jack_set_buffer_size_callback`: a callback for the server's block changing.
        @param client  the client
        @param callback  called, on a thread of the server's, with the new block
        @param arg  passed to `callback`
        @return  0 on success; -1 with no library or on failure */
    int reckless_jack_set_buffer_size_callback(jack_client_t *client, JackBufferSizeCallback callback, void *arg)
    {
        const auto function = Library().set_buffer_size_callback;
        return function != nullptr ? function(client, callback, arg) : -1;
    }

    /** Whether the libjack loaded is PipeWire's - the graph's own JACK - rather than a JACK server's. Not libjack's:
       the loader's own answer, from what the library links.
        @return  1 for PipeWire's libjack; 0 for JACK's own, or none */
    int reckless_jack_is_pipewire(void)
    {
        return Library().is_pipewire ? 1 : 0;
    }

    /** libjack's `jack_set_xrun_callback`: the function that is told of an xrun.
        @param client  the client
        @param xrun_callback  called when the server reports an xrun
        @param arg  passed to `xrun_callback`
        @return  0 on success; -1 with no library or on failure */
    int reckless_jack_set_xrun_callback(jack_client_t *client, JackXRunCallback xrun_callback, void *arg)
    {
        const auto function = Library().set_xrun_callback;
        return function != nullptr ? function(client, xrun_callback, arg) : -1;
    }
}
