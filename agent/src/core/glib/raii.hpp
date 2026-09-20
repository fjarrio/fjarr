#pragma once
// The RAII kit: the ONLY place in libfjarr that touches GObject/GstMiniObject
// reference counts and GLib source ids (a /verify grep gate enforces it).
// spec: docs/23-agent-core-architecture.md#memory-and-lifetime-discipline-and-the-tooling-that-enforces-it
// heritage: the camera streamer's webrtc_common.hpp (docs/11), extended.
#include <atomic>
#include <exception>
#include <functional>
#include <memory>
#include <string>
#include <utility>

#include <gst/gst.h>
#include <gst/sdp/sdp.h>
#include <gst/webrtc/webrtc.h>

namespace fjarr::glib {

// ------------------------------------------------------------ census

/// Live wrapper counts by type (docs/24 `/memory`). Negligible cost.
struct ObjectCensus {
    std::atomic<long> elements{0}, pads{0}, samples{0}, buffers{0}, promises{0}, sources{0},
        signals{0}, probes{0}, sessions{0}, pipelines{0};
    static ObjectCensus& instance();
};

// -------------------------------------------------------- deleters

struct GObjectUnref {
    template <class T> void operator()(T* p) const noexcept {
        if (p) g_object_unref(p);
    }
};
struct GstObjectUnref {
    template <class T> void operator()(T* p) const noexcept {
        if (p) gst_object_unref(p);
    }
};
struct ElementUnref {
    void operator()(GstElement* p) const noexcept {
        if (p) {
            gst_object_unref(p);
            ObjectCensus::instance().elements--;
        }
    }
};
struct PadUnref {
    void operator()(GstPad* p) const noexcept {
        if (p) {
            gst_object_unref(p);
            ObjectCensus::instance().pads--;
        }
    }
};
struct CapsUnref {
    void operator()(GstCaps* p) const noexcept {
        if (p) gst_caps_unref(p);
    }
};
struct SampleUnref {
    void operator()(GstSample* p) const noexcept {
        if (p) {
            gst_sample_unref(p);
            ObjectCensus::instance().samples--;
        }
    }
};
struct BufferUnref {
    void operator()(GstBuffer* p) const noexcept {
        if (p) {
            gst_buffer_unref(p);
            ObjectCensus::instance().buffers--;
        }
    }
};
struct MessageUnref {
    void operator()(GstMessage* p) const noexcept {
        if (p) gst_message_unref(p);
    }
};
struct EventUnref {
    void operator()(GstEvent* p) const noexcept {
        if (p) gst_event_unref(p);
    }
};
struct StructureFree {
    void operator()(GstStructure* p) const noexcept {
        if (p) gst_structure_free(p);
    }
};
struct PromiseUnref {
    void operator()(GstPromise* p) const noexcept {
        if (p) {
            gst_promise_unref(p);
            ObjectCensus::instance().promises--;
        }
    }
};
struct SdpFree {
    void operator()(GstWebRTCSessionDescription* p) const noexcept {
        if (p) gst_webrtc_session_description_free(p);
    }
};
struct GFree {
    void operator()(void* p) const noexcept { g_free(p); }
};
struct GErrorFree {
    void operator()(GError* p) const noexcept {
        if (p) g_error_free(p);
    }
};
struct GBytesUnref {
    void operator()(GBytes* p) const noexcept {
        if (p) g_bytes_unref(p);
    }
};

// ---------------------------------------------------------- pointers

template <class T> using GObjectPtr = std::unique_ptr<T, GObjectUnref>;
template <class T> using GstObjectPtr = std::unique_ptr<T, GstObjectUnref>;
using GstElementPtr = std::unique_ptr<GstElement, ElementUnref>;
using GstPadPtr = std::unique_ptr<GstPad, PadUnref>;
using GstBusPtr = GstObjectPtr<GstBus>;
using GstCapsPtr = std::unique_ptr<GstCaps, CapsUnref>;
using GstSamplePtr = std::unique_ptr<GstSample, SampleUnref>;
using GstBufferPtr = std::unique_ptr<GstBuffer, BufferUnref>;
using GstMessagePtr = std::unique_ptr<GstMessage, MessageUnref>;
using GstEventPtr = std::unique_ptr<GstEvent, EventUnref>;
using GstStructurePtr = std::unique_ptr<GstStructure, StructureFree>;
using GstPromisePtr = std::unique_ptr<GstPromise, PromiseUnref>;
using SdpPtr = std::unique_ptr<GstWebRTCSessionDescription, SdpFree>;
using GStrPtr = std::unique_ptr<gchar, GFree>;
using GErrorPtr = std::unique_ptr<GError, GErrorFree>;
using GBytesPtr = std::unique_ptr<GBytes, GBytesUnref>;

/// Take ownership of a (transfer full) element; counts it.
[[nodiscard]] inline GstElementPtr adopt_element(GstElement* e) {
    if (e) ObjectCensus::instance().elements++;
    return GstElementPtr(e);
}
/// Take ownership of a floating element (sinks the floating ref).
[[nodiscard]] inline GstElementPtr sink_element(GstElement* e) {
    if (e) gst_object_ref_sink(e);
    return adopt_element(e);
}
[[nodiscard]] inline GstElementPtr make_element(const char* factory, const std::string& name) {
    GstElement* e = gst_element_factory_make(factory, name.empty() ? nullptr : name.c_str());
    return sink_element(e);
}
/// A new reference to an element someone else owns (e.g. a bin child).
[[nodiscard]] inline GstElementPtr ref_element(GstElement* e) {
    if (e) gst_object_ref(e);
    return adopt_element(e);
}
[[nodiscard]] inline GstPadPtr adopt_pad(GstPad* p) {
    if (p) ObjectCensus::instance().pads++;
    return GstPadPtr(p);
}
[[nodiscard]] inline GstPadPtr ref_pad(GstPad* p) {
    if (p) gst_object_ref(p);
    return adopt_pad(p);
}
[[nodiscard]] inline GstSamplePtr adopt_sample(GstSample* s) {
    if (s) ObjectCensus::instance().samples++;
    return GstSamplePtr(s);
}
[[nodiscard]] inline GstSamplePtr ref_sample(GstSample* s) {
    if (s) gst_sample_ref(s);
    return adopt_sample(s);
}
[[nodiscard]] inline GstBufferPtr adopt_buffer(GstBuffer* b) {
    if (b) ObjectCensus::instance().buffers++;
    return GstBufferPtr(b);
}
[[nodiscard]] inline GstBufferPtr ref_buffer(GstBuffer* b) {
    if (b) gst_buffer_ref(b);
    return adopt_buffer(b);
}
[[nodiscard]] inline GstPromisePtr adopt_promise(GstPromise* p) {
    if (p) ObjectCensus::instance().promises++;
    return GstPromisePtr(p);
}
[[nodiscard]] inline GstCapsPtr adopt_caps(GstCaps* c) { return GstCapsPtr(c); }
[[nodiscard]] inline GstCapsPtr ref_caps(GstCaps* c) {
    if (c) gst_caps_ref(c);
    return GstCapsPtr(c);
}
/// Release a buffer to a C API that takes ownership (appsrc push).
[[nodiscard]] inline GstBuffer* release_buffer(GstBufferPtr b) {
    if (b) ObjectCensus::instance().buffers--;
    return b.release();
}

// ----------------------------------------------------------- signals

/// A g_signal_connect that disconnects itself when destroyed, holding only a
/// weak reference to the instance (the instance may go first).
class SignalConnection {
  public:
    SignalConnection() = default;
    SignalConnection(gpointer instance, const char* detailed_signal, GCallback handler,
                     gpointer data, GClosureNotify destroy = nullptr) {
        g_weak_ref_init(&ref_, instance);
        id_ = g_signal_connect_data(instance, detailed_signal, handler, data, destroy,
                                    static_cast<GConnectFlags>(0));
        if (id_ != 0) ObjectCensus::instance().signals++;
        else g_weak_ref_clear(&ref_);
    }
    ~SignalConnection() { disconnect(); }
    SignalConnection(SignalConnection&& o) noexcept { *this = std::move(o); }
    SignalConnection& operator=(SignalConnection&& o) noexcept {
        if (this != &o) {
            disconnect();
            if (o.id_ != 0) {
                gpointer inst = g_weak_ref_get(&o.ref_);
                g_weak_ref_init(&ref_, inst);
                if (inst) g_object_unref(inst);
                id_ = o.id_;
                g_weak_ref_clear(&o.ref_);
                o.id_ = 0;
            }
        }
        return *this;
    }
    SignalConnection(const SignalConnection&) = delete;
    SignalConnection& operator=(const SignalConnection&) = delete;

    void disconnect() noexcept {
        if (id_ == 0) return;
        gpointer inst = g_weak_ref_get(&ref_);
        if (inst) {
            if (g_signal_handler_is_connected(inst, id_)) g_signal_handler_disconnect(inst, id_);
            g_object_unref(inst);
        }
        g_weak_ref_clear(&ref_);
        id_ = 0;
        ObjectCensus::instance().signals--;
    }
    explicit operator bool() const noexcept { return id_ != 0; }

  private:
    GWeakRef ref_{};
    gulong id_ = 0;
};

// ------------------------------------------------------------ probes

/// A pad probe removed when destroyed.
class PadProbe {
  public:
    PadProbe() = default;
    PadProbe(GstPad* pad, GstPadProbeType mask, GstPadProbeCallback cb, gpointer data,
             GDestroyNotify destroy = nullptr)
        : pad_(ref_pad(pad)) {
        id_ = gst_pad_add_probe(pad, mask, cb, data, destroy);
        ObjectCensus::instance().probes++;
    }
    ~PadProbe() { remove(); }
    PadProbe(PadProbe&& o) noexcept { *this = std::move(o); }
    PadProbe& operator=(PadProbe&& o) noexcept {
        if (this != &o) {
            remove();
            pad_ = std::move(o.pad_);
            id_ = std::exchange(o.id_, 0);
        }
        return *this;
    }
    PadProbe(const PadProbe&) = delete;
    PadProbe& operator=(const PadProbe&) = delete;
    void remove() noexcept {
        if (id_ != 0 && pad_) {
            gst_pad_remove_probe(pad_.get(), id_);
            ObjectCensus::instance().probes--;
        }
        id_ = 0;
        pad_.reset();
    }
    explicit operator bool() const noexcept { return id_ != 0; }

  private:
    GstPadPtr pad_;
    gulong id_ = 0;
};

// ----------------------------------------------------------- sources

/// A GLib source attached to a context; destroyed with the guard. The
/// callback is a std::function owned by the source.
class SourceGuard {
  public:
    SourceGuard() = default;
    /// Takes ownership of a new (unattached) source and attaches it.
    SourceGuard(GSource* source, GMainContext* ctx, std::function<bool()> fn) {
        auto* boxed = new std::function<bool()>(std::move(fn));
        g_source_set_callback(
            source,
            [](gpointer d) -> gboolean {
                // Last line of defence: an exception cannot unwind through GLib's dispatch (docs/23 threading model).
                try {
                    return (*static_cast<std::function<bool()>*>(d))() ? G_SOURCE_CONTINUE : G_SOURCE_REMOVE;
                } catch (const std::exception& e) {
                    g_critical("fjarr: loop callback threw: %s", e.what());
                    return G_SOURCE_REMOVE;
                }
            },
            boxed, [](gpointer d) { delete static_cast<std::function<bool()>*>(d); });
        g_source_attach(source, ctx);
        source_ = source; // attach took its own ref; we keep ours for destroy
        ObjectCensus::instance().sources++;
    }
    /// Take ownership of a source that is already configured and attached
    /// (bus watches: gst_bus_create_watch + g_source_set_callback).
    static SourceGuard attached(GSource* source) {
        SourceGuard g;
        g.source_ = source;
        ObjectCensus::instance().sources++;
        return g;
    }
    ~SourceGuard() { cancel(); }
    SourceGuard(SourceGuard&& o) noexcept { *this = std::move(o); }
    SourceGuard& operator=(SourceGuard&& o) noexcept {
        if (this != &o) {
            cancel();
            source_ = std::exchange(o.source_, nullptr);
        }
        return *this;
    }
    SourceGuard(const SourceGuard&) = delete;
    SourceGuard& operator=(const SourceGuard&) = delete;
    void cancel() noexcept {
        if (!source_) return;
        if (!g_source_is_destroyed(source_)) g_source_destroy(source_);
        g_source_unref(source_);
        source_ = nullptr;
        ObjectCensus::instance().sources--;
    }
    explicit operator bool() const noexcept { return source_ != nullptr && !g_source_is_destroyed(source_); }

  private:
    GSource* source_ = nullptr;
};

/// Fire-and-forget: run `fn` once on `ctx` (the callback owns its closure).
inline void invoke_once(GMainContext* ctx, std::function<void()> fn, int priority = G_PRIORITY_DEFAULT) {
    auto* boxed = new std::function<void()>(std::move(fn));
    GSource* s = g_idle_source_new();
    g_source_set_priority(s, priority);
    g_source_set_callback(
        s,
        [](gpointer d) -> gboolean {
            try {
                (*static_cast<std::function<void()>*>(d))();
            } catch (const std::exception& e) {
                g_critical("fjarr: posted callback threw: %s", e.what());
            }
            return G_SOURCE_REMOVE;
        },
        boxed, [](gpointer d) { delete static_cast<std::function<void()>*>(d); });
    g_source_attach(s, ctx);
    g_source_unref(s);
}

/// Take an extra reference on any GObject (GstObject or plain) and return it, for
/// handing a borrowed callback argument to a posted closure. The only ref() site
/// outside the kit's own wrappers (the Makefile gate enforces that).
template <class T>
[[nodiscard]] inline T* ref_object(T* o) {
    g_object_ref(o);
    return o;
}

/// A promise whose change func marshals the reply (copied) to a std::function.
/// webrtcbin's action signals only *ref* the promise they are given, so the
/// caller's reference is released by the change func itself once the promise
/// settles (replied, interrupted or expired) — pass the result straight to
/// `g_signal_emit_by_name` and forget it (the camera streamer's idiom).
[[nodiscard]] inline GstPromise* make_promise(std::function<void(GstPromiseResult, GstStructurePtr)> on_reply) {
    auto* boxed = new std::function<void(GstPromiseResult, GstStructurePtr)>(std::move(on_reply));
    GstPromise* p = gst_promise_new_with_change_func(
        [](GstPromise* pr, gpointer d) {
            auto* fn = static_cast<std::function<void(GstPromiseResult, GstStructurePtr)>*>(d);
            const GstStructure* reply = gst_promise_get_reply(pr);
            const GstPromiseResult result = gst_promise_wait(pr);
            (*fn)(result, GstStructurePtr(reply ? gst_structure_copy(reply) : nullptr));
            // gst_promise_expire() clears change_data before calling here, so the notify below
            // would run with NULL: the closure is ours to free on that path.
            if (result == GST_PROMISE_RESULT_EXPIRED) delete fn;
            ObjectCensus::instance().promises--;
            gst_promise_unref(pr); // the caller's reference; the emitter dropped its own
        },
        boxed, [](gpointer d) { delete static_cast<std::function<void(GstPromiseResult, GstStructurePtr)>*>(d); });
    ObjectCensus::instance().promises++;
    return p;
}

/// Scope guard.
class ScopeGuard {
  public:
    explicit ScopeGuard(std::function<void()> fn) : fn_(std::move(fn)) {}
    ~ScopeGuard() {
        if (fn_) fn_();
    }
    void dismiss() { fn_ = nullptr; }
    ScopeGuard(const ScopeGuard&) = delete;
    ScopeGuard& operator=(const ScopeGuard&) = delete;

  private:
    std::function<void()> fn_;
};

// ---------------------------------------------------------- helpers

inline std::string str_prop(gpointer obj, const char* name) {
    if (!g_object_class_find_property(G_OBJECT_GET_CLASS(obj), name)) return {};
    gchar* s = nullptr;
    g_object_get(obj, name, &s, nullptr);
    GStrPtr guard(s);
    return s ? std::string(s) : std::string();
}
inline std::string enum_nick(GType type, gint value) {
    auto* k = static_cast<GEnumClass*>(g_type_class_ref(type));
    GEnumValue* e = g_enum_get_value(k, value);
    std::string s = e ? e->value_nick : "?";
    g_type_class_unref(k);
    return s;
}
inline std::string enum_prop_nick(gpointer obj, const char* name) {
    GParamSpec* ps = g_object_class_find_property(G_OBJECT_GET_CLASS(obj), name);
    if (!ps || !G_IS_PARAM_SPEC_ENUM(ps)) return {};
    gint v = 0;
    g_object_get(obj, name, &v, nullptr);
    return enum_nick(ps->value_type, v);
}
inline std::string element_name(GstElement* e) {
    GStrPtr n(gst_element_get_name(e));
    return n ? std::string(n.get()) : std::string();
}
inline std::string pad_name(GstPad* p) {
    GStrPtr n(gst_pad_get_name(p));
    return n ? std::string(n.get()) : std::string();
}
inline std::string caps_to_string(const GstCaps* c) {
    if (!c) return {};
    GStrPtr s(gst_caps_to_string(c));
    return s ? std::string(s.get()) : std::string();
}

} // namespace fjarr::glib
