/*
 * Copyright (c) 2022, Andrew Kaster <akaster@serenityos.org>
 * Copyright (c) 2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Base64.h>
#include <AK/QuickSort.h>
#include <AK/String.h>
#include <AK/Utf8View.h>
#include <AK/Vector.h>
#include <LibJS/Heap/HeapFunction.h>
#include <LibJS/Runtime/Array.h>
#include <LibTextCodec/Decoder.h>
#include <LibWeb/Bindings/MainThreadVM.h>
#include <LibWeb/Crypto/Crypto.h>
#include <LibWeb/Fetch/FetchMethod.h>
#include <LibWeb/HTML/CanvasRenderingContext2D.h>
#include <LibWeb/HTML/ErrorEvent.h>
#include <LibWeb/HTML/EventLoop/EventLoop.h>
#include <LibWeb/HTML/EventSource.h>
#include <LibWeb/HTML/ImageBitmap.h>
#include <LibWeb/HTML/PromiseRejectionEvent.h>
#include <LibWeb/HTML/Scripting/ClassicScript.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Scripting/ExceptionReporter.h>
#include <LibWeb/HTML/Scripting/Fetching.h>
#include <LibWeb/HTML/StructuredSerialize.h>
#include <LibWeb/HTML/StructuredSerializeOptions.h>
#include <LibWeb/HTML/Timer.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/HTML/WindowOrWorkerGlobalScope.h>
#include <LibWeb/HighResolutionTime/Performance.h>
#include <LibWeb/HighResolutionTime/SupportedPerformanceTypes.h>
#include <LibWeb/IndexedDB/IDBFactory.h>
#include <LibWeb/Infra/Base64.h>
#include <LibWeb/PerformanceTimeline/EntryTypes.h>
#include <LibWeb/PerformanceTimeline/PerformanceObserver.h>
#include <LibWeb/PerformanceTimeline/PerformanceObserverEntryList.h>
#include <LibWeb/Platform/EventLoopPlugin.h>
#include <LibWeb/Platform/ImageCodecPlugin.h>
#include <LibWeb/UserTiming/PerformanceMark.h>
#include <LibWeb/UserTiming/PerformanceMeasure.h>
#include <LibWeb/WebIDL/AbstractOperations.h>
#include <LibWeb/WebIDL/DOMException.h>
#include <LibWeb/WebIDL/ExceptionOr.h>
#include <LibWeb/WebIDL/Types.h>

namespace Web::HTML {

WindowOrWorkerGlobalScopeMixin::~WindowOrWorkerGlobalScopeMixin() = default;

void WindowOrWorkerGlobalScopeMixin::initialize(JS::Realm&)
{
#define __ENUMERATE_SUPPORTED_PERFORMANCE_ENTRY_TYPES(entry_type, cpp_class) \
    m_performance_entry_buffer_map.set(entry_type,                           \
        PerformanceTimeline::PerformanceEntryTuple {                         \
            .performance_entry_buffer = {},                                  \
            .max_buffer_size = cpp_class::max_buffer_size(),                 \
            .available_from_timeline = cpp_class::available_from_timeline(), \
            .dropped_entries_count = 0,                                      \
        });
    ENUMERATE_SUPPORTED_PERFORMANCE_ENTRY_TYPES
#undef __ENUMERATE_SUPPORTED_PERFORMANCE_ENTRY_TYPES
}

void WindowOrWorkerGlobalScopeMixin::visit_edges(JS::Cell::Visitor& visitor)
{
    visitor.visit(m_performance);
    visitor.visit(m_supported_entry_types_array);
    visitor.visit(m_timers);
    visitor.visit(m_registered_performance_observer_objects);
    visitor.visit(m_indexed_db);
    for (auto& entry : m_performance_entry_buffer_map)
        entry.value.visit_edges(visitor);
    visitor.visit(m_registered_event_sources);
    visitor.visit(m_crypto);
    visitor.ignore(m_outstanding_rejected_promises_weak_set);
}

void WindowOrWorkerGlobalScopeMixin::finalize()
{
    clear_map_of_active_timers();
}

// https://html.spec.whatwg.org/multipage/webappapis.html#dom-origin
WebIDL::ExceptionOr<String> WindowOrWorkerGlobalScopeMixin::origin() const
{
    auto& vm = this_impl().vm();

    // The origin getter steps are to return this's relevant settings object's origin, serialized.
    return TRY_OR_THROW_OOM(vm, String::from_byte_string(relevant_settings_object(this_impl()).origin().serialize()));
}

// https://html.spec.whatwg.org/multipage/webappapis.html#dom-issecurecontext
bool WindowOrWorkerGlobalScopeMixin::is_secure_context() const
{
    // The isSecureContext getter steps are to return true if this's relevant settings object is a secure context, or false otherwise.
    return HTML::is_secure_context(relevant_settings_object(this_impl()));
}

// https://html.spec.whatwg.org/multipage/webappapis.html#dom-crossoriginisolated
bool WindowOrWorkerGlobalScopeMixin::cross_origin_isolated() const
{
    // The crossOriginIsolated getter steps are to return this's relevant settings object's cross-origin isolated capability.
    return relevant_settings_object(this_impl()).cross_origin_isolated_capability() == CanUseCrossOriginIsolatedAPIs::Yes;
}

// https://html.spec.whatwg.org/multipage/webappapis.html#dom-btoa
WebIDL::ExceptionOr<String> WindowOrWorkerGlobalScopeMixin::btoa(String const& data) const
{
    auto& vm = this_impl().vm();
    auto& realm = *vm.current_realm();

    // The btoa(data) method must throw an "InvalidCharacterError" DOMException if data contains any character whose code point is greater than U+00FF.
    Vector<u8> byte_string;
    byte_string.ensure_capacity(data.bytes().size());
    for (u32 code_point : Utf8View(data)) {
        if (code_point > 0xff)
            return WebIDL::InvalidCharacterError::create(realm, "Data contains characters outside the range U+0000 and U+00FF"_string);
        byte_string.append(code_point);
    }

    // Otherwise, the user agent must convert data to a byte sequence whose nth byte is the eight-bit representation of the nth code point of data,
    // and then must apply forgiving-base64 encode to that byte sequence and return the result.
    return TRY_OR_THROW_OOM(vm, encode_base64(byte_string.span()));
}

// https://html.spec.whatwg.org/multipage/webappapis.html#dom-atob
WebIDL::ExceptionOr<String> WindowOrWorkerGlobalScopeMixin::atob(String const& data) const
{
    auto& vm = this_impl().vm();
    auto& realm = *vm.current_realm();

    // 1. Let decodedData be the result of running forgiving-base64 decode on data.
    auto decoded_data = Infra::decode_forgiving_base64(data.bytes_as_string_view());

    // 2. If decodedData is failure, then throw an "InvalidCharacterError" DOMException.
    if (decoded_data.is_error())
        return WebIDL::InvalidCharacterError::create(realm, "Input string is not valid base64 data"_string);

    // 3. Return decodedData.
    // decode_base64() returns a byte string. LibJS uses UTF-8 for strings. Use Latin1Decoder to convert bytes 128-255 to UTF-8.
    auto decoder = TextCodec::decoder_for_exact_name("ISO-8859-1"sv);
    VERIFY(decoder.has_value());
    return TRY_OR_THROW_OOM(vm, decoder->to_utf8(decoded_data.value()));
}

// https://html.spec.whatwg.org/multipage/timers-and-user-prompts.html#dom-queuemicrotask
void WindowOrWorkerGlobalScopeMixin::queue_microtask(WebIDL::CallbackType& callback)
{
    auto& vm = this_impl().vm();
    auto& realm = *vm.current_realm();

    JS::GCPtr<DOM::Document> document;
    if (is<Window>(this_impl()))
        document = &static_cast<Window&>(this_impl()).associated_document();

    // The queueMicrotask(callback) method must queue a microtask to invoke callback, and if callback throws an exception, report the exception.
    HTML::queue_a_microtask(document, JS::create_heap_function(realm.heap(), [&callback, &realm] {
        auto result = WebIDL::invoke_callback(callback, {});
        if (result.is_error())
            HTML::report_exception(result, realm);
    }));
}

// https://html.spec.whatwg.org/multipage/imagebitmap-and-animations.html#dom-createimagebitmap
JS::NonnullGCPtr<JS::Promise> WindowOrWorkerGlobalScopeMixin::create_image_bitmap(ImageBitmapSource image, Optional<ImageBitmapOptions> options) const
{
    return create_image_bitmap_impl(image, {}, {}, {}, {}, options);
}

// https://html.spec.whatwg.org/multipage/imagebitmap-and-animations.html#dom-createimagebitmap
JS::NonnullGCPtr<JS::Promise> WindowOrWorkerGlobalScopeMixin::create_image_bitmap(ImageBitmapSource image, WebIDL::Long sx, WebIDL::Long sy, WebIDL::Long sw, WebIDL::Long sh, Optional<ImageBitmapOptions> options) const
{
    return create_image_bitmap_impl(image, sx, sy, sw, sh, options);
}

JS::NonnullGCPtr<JS::Promise> WindowOrWorkerGlobalScopeMixin::create_image_bitmap_impl(ImageBitmapSource& image, Optional<WebIDL::Long> sx, Optional<WebIDL::Long> sy, Optional<WebIDL::Long> sw, Optional<WebIDL::Long> sh, Optional<ImageBitmapOptions>& options) const
{
    // 1. If either sw or sh is given and is 0, then return a promise rejected with a RangeError.
    if (sw == 0 || sh == 0) {
        auto promise = JS::Promise::create(this_impl().realm());
        auto error_message = MUST(String::formatted("{} is an invalid value for {}", sw == 0 ? *sw : *sh, sw == 0 ? "sw"sv : "sh"sv));
        promise->reject(JS::RangeError::create(this_impl().realm(), move(error_message)));
        return promise;
    }

    // FIXME:
    // 2. If either options's resizeWidth or options's resizeHeight is present and is 0, then return a promise rejected with an "InvalidStateError" DOMException.
    (void)options;

    // 3. Check the usability of the image argument. If this throws an exception or returns bad, then return a promise rejected with an "InvalidStateError" DOMException.
    // FIXME: "Check the usability of the image argument" is only defined for CanvasImageSource, let's skip it for other types
    if (image.has<CanvasImageSource>()) {
        if (auto usability = check_usability_of_image(image.get<CanvasImageSource>()); usability.is_error() or usability.value() == CanvasImageSourceUsability::Bad) {
            auto promise = JS::Promise::create(this_impl().realm());
            promise->reject(WebIDL::InvalidStateError::create(this_impl().realm(), "image argument is not usable"_string));
            return promise;
        }
    }

    // 4. Let p be a new promise.
    auto p = JS::Promise::create(this_impl().realm());

    // 5. Let imageBitmap be a new ImageBitmap object.
    auto image_bitmap = ImageBitmap::create(this_impl().realm());

    // 6. Switch on image:
    image.visit(
        [&](JS::Handle<FileAPI::Blob>& blob) {
            // Run these step in parallel:
            Platform::EventLoopPlugin::the().deferred_invoke([=]() {
                // 1. Let imageData be the result of reading image's data. If an error occurs during reading of the
                // object, then reject p with an "InvalidStateError" DOMException and abort these steps.
                // FIXME: I guess this is always fine for us as the data is already read.
                auto const image_data = blob->raw_bytes();

                // FIXME:
                // 2. Apply the image sniffing rules to determine the file format of imageData, with MIME type of
                // image (as given by image's type attribute) giving the official type.

                auto on_failed_decode = [p = JS::Handle(*p)](Error&) {
                    // 3. If imageData is not in a supported image file format (e.g., it's not an image at all), or if
                    // imageData is corrupted in some fatal way such that the image dimensions cannot be obtained
                    // (e.g., a vector graphic with no natural size), then reject p with an "InvalidStateError" DOMException
                    // and abort these steps.
                    p->reject(WebIDL::InvalidStateError::create(relevant_realm(*p), "image does not contain a supported image format"_string));
                };

                auto on_successful_decode = [image_bitmap = JS::Handle(*image_bitmap), p = JS::Handle(*p)](Web::Platform::DecodedImage& result) -> ErrorOr<void> {
                    // 4. Set imageBitmap's bitmap data to imageData, cropped to the source rectangle with formatting.
                    // If this is an animated image, imageBitmap's bitmap data must only be taken from the default image
                    // of the animation (the one that the format defines is to be used when animation is not supported
                    // or is disabled), or, if there is no such image, the first frame of the animation.
                    image_bitmap->set_bitmap(result.frames.take_first().bitmap);

                    // 5. Resolve p with imageBitmap.
                    p->fulfill(image_bitmap);
                    return {};
                };

                (void)Web::Platform::ImageCodecPlugin::the().decode_image(image_data, move(on_successful_decode), move(on_failed_decode));
            });
        },
        [&](auto&) {
            dbgln("(STUBBED) createImageBitmap() for non-blob types");
            (void)sx;
            (void)sy;
            p->reject(JS::Error::create(relevant_realm(*p), "Not Implemented: createImageBitmap() for non-blob types"sv));
        });

    // 7. Return p.
    return p;
}

// https://html.spec.whatwg.org/multipage/structured-data.html#dom-structuredclone
WebIDL::ExceptionOr<JS::Value> WindowOrWorkerGlobalScopeMixin::structured_clone(JS::Value value, StructuredSerializeOptions const& options) const
{
    auto& vm = this_impl().vm();
    (void)options;

    // 1. Let serialized be ? StructuredSerializeWithTransfer(value, options["transfer"]).
    // FIXME: Use WithTransfer variant of the AO
    auto serialized = TRY(structured_serialize(vm, value));

    // 2. Let deserializeRecord be ? StructuredDeserializeWithTransfer(serialized, this's relevant realm).
    // FIXME: Use WithTransfer variant of the AO
    auto deserialized = TRY(structured_deserialize(vm, serialized, relevant_realm(this_impl()), {}));

    // 3. Return deserializeRecord.[[Deserialized]].
    return deserialized;
}

JS::NonnullGCPtr<JS::Promise> WindowOrWorkerGlobalScopeMixin::fetch(Fetch::RequestInfo const& input, Fetch::RequestInit const& init) const
{
    auto& vm = this_impl().vm();
    return Fetch::fetch(vm, input, init);
}

// https://html.spec.whatwg.org/multipage/timers-and-user-prompts.html#dom-settimeout
i32 WindowOrWorkerGlobalScopeMixin::set_timeout(TimerHandler handler, i32 timeout, JS::MarkedVector<JS::Value> arguments)
{
    return run_timer_initialization_steps(move(handler), timeout, move(arguments), Repeat::No);
}

// https://html.spec.whatwg.org/multipage/timers-and-user-prompts.html#dom-setinterval
i32 WindowOrWorkerGlobalScopeMixin::set_interval(TimerHandler handler, i32 timeout, JS::MarkedVector<JS::Value> arguments)
{
    return run_timer_initialization_steps(move(handler), timeout, move(arguments), Repeat::Yes);
}

// https://html.spec.whatwg.org/multipage/timers-and-user-prompts.html#dom-cleartimeout
void WindowOrWorkerGlobalScopeMixin::clear_timeout(i32 id)
{
    if (auto timer = m_timers.get(id); timer.has_value())
        timer.value()->stop();
    m_timers.remove(id);
}

// https://html.spec.whatwg.org/multipage/timers-and-user-prompts.html#dom-clearinterval
void WindowOrWorkerGlobalScopeMixin::clear_interval(i32 id)
{
    if (auto timer = m_timers.get(id); timer.has_value())
        timer.value()->stop();
    m_timers.remove(id);
}

void WindowOrWorkerGlobalScopeMixin::clear_map_of_active_timers()
{
    for (auto& it : m_timers)
        it.value->stop();
    m_timers.clear();
}

// https://html.spec.whatwg.org/multipage/timers-and-user-prompts.html#timer-initialisation-steps
// With no active script fix from https://github.com/whatwg/html/pull/9712
i32 WindowOrWorkerGlobalScopeMixin::run_timer_initialization_steps(TimerHandler handler, i32 timeout, JS::MarkedVector<JS::Value> arguments, Repeat repeat, Optional<i32> previous_id)
{
    // 1. Let thisArg be global if that is a WorkerGlobalScope object; otherwise let thisArg be the WindowProxy that corresponds to global.

    // 2. If previousId was given, let id be previousId; otherwise, let id be an implementation-defined integer that is greater than zero and does not already exist in global's map of active timers.
    auto id = previous_id.has_value() ? previous_id.value() : m_timer_id_allocator.allocate();

    // FIXME: 3. If the surrounding agent's event loop's currently running task is a task that was created by this algorithm, then let nesting level be the task's timer nesting level. Otherwise, let nesting level be zero.

    // 4. If timeout is less than 0, then set timeout to 0.
    if (timeout < 0)
        timeout = 0;

    // FIXME: 5. If nesting level is greater than 5, and timeout is less than 4, then set timeout to 4.

    // 6. Let callerRealm be the current Realm Record, and calleeRealm be global's relevant Realm.
    // FIXME: Implement this when step 9.3.2 is implemented.

    // 7. Let initiating script be the active script.
    auto const* initiating_script = Web::Bindings::active_script();

    auto& vm = this_impl().vm();

    // 8. Let task be a task that runs the following substeps:
    auto task = JS::create_heap_function(vm.heap(), Function<void()>([this, handler = move(handler), timeout, arguments = move(arguments), repeat, id, initiating_script]() {
        // 1. If id does not exist in global's map of active timers, then abort these steps.
        if (!m_timers.contains(id))
            return;

        handler.visit(
            // 2. If handler is a Function, then invoke handler given arguments with the callback this value set to thisArg. If this throws an exception, catch it, and report the exception.
            [&](JS::Handle<WebIDL::CallbackType> const& callback) {
                if (auto result = WebIDL::invoke_callback(*callback, &this_impl(), arguments); result.is_error())
                    report_exception(result, this_impl().realm());
            },
            // 3. Otherwise:
            [&](String const& source) {
                // 1. Assert: handler is a string.
                // FIXME: 2. Perform HostEnsureCanCompileStrings(callerRealm, calleeRealm). If this throws an exception, catch it, report the exception, and abort these steps.

                // 3. Let settings object be global's relevant settings object.
                auto& settings_object = relevant_settings_object(this_impl());

                // 4. Let fetch options be the default classic script fetch options.
                ScriptFetchOptions options {};

                // 5. Let base URL be settings object's API base URL.
                auto base_url = settings_object.api_base_url();

                // 6. If initiating script is not null, then:
                if (initiating_script) {
                    // FIXME: 1. Set fetch options to a script fetch options whose cryptographic nonce is initiating script's fetch options's cryptographic nonce,
                    //           integrity metadata is the empty string, parser metadata is "not-parser-inserted", credentials mode is initiating script's fetch
                    //           options's credentials mode, referrer policy is initiating script's fetch options's referrer policy, and fetch priority is "auto".

                    // 2. Set base URL to initiating script's base URL.
                    base_url = initiating_script->base_url();

                    // Spec Note: The effect of these steps ensures that the string compilation done by setTimeout() and setInterval() behaves equivalently to that
                    //            done by eval(). That is, module script fetches via import() will behave the same in both contexts.
                }

                // 7. Let script be the result of creating a classic script given handler, settings object, base URL, and fetch options.
                // FIXME: Pass fetch options.
                auto basename = base_url.basename();
                auto script = ClassicScript::create(basename, source, settings_object, move(base_url));

                // 8. Run the classic script script.
                (void)script->run();
            });

        // 4. If id does not exist in global's map of active timers, then abort these steps.
        if (!m_timers.contains(id))
            return;

        switch (repeat) {
        // 5. If repeat is true, then perform the timer initialization steps again, given global, handler, timeout, arguments, true, and id.
        case Repeat::Yes:
            run_timer_initialization_steps(handler, timeout, move(arguments), repeat, id);
            break;

        // 6. Otherwise, remove global's map of active timers[id].
        case Repeat::No:
            m_timers.remove(id);
            break;
        }
    }));

    // FIXME: 9. Increment nesting level by one.
    // FIXME: 10. Set task's timer nesting level to nesting level.

    // 11. Let completionStep be an algorithm step which queues a global task on the timer task source given global to run task.
    Function<void()> completion_step = [this, task = move(task)]() mutable {
        queue_global_task(Task::Source::TimerTask, this_impl(), JS::create_heap_function(this_impl().heap(), [task] {
            task->function()();
        }));
    };

    // 12. Run steps after a timeout given global, "setTimeout/setInterval", timeout, completionStep, and id.
    run_steps_after_a_timeout_impl(timeout, move(completion_step), id);

    // 13. Return id.
    return id;
}

// 1. https://www.w3.org/TR/performance-timeline/#dfn-relevant-performance-entry-tuple
PerformanceTimeline::PerformanceEntryTuple& WindowOrWorkerGlobalScopeMixin::relevant_performance_entry_tuple(FlyString const& entry_type)
{
    // 1. Let map be the performance entry buffer map associated with globalObject.
    // 2. Return the result of getting the value of an entry from map, given entryType as the key.
    auto tuple = m_performance_entry_buffer_map.get(entry_type);

    // This shouldn't be called with entry types that aren't in `ENUMERATE_SUPPORTED_PERFORMANCE_ENTRY_TYPES`.
    VERIFY(tuple.has_value());
    return tuple.value();
}

// https://www.w3.org/TR/performance-timeline/#dfn-queue-a-performanceentry
void WindowOrWorkerGlobalScopeMixin::queue_performance_entry(JS::NonnullGCPtr<PerformanceTimeline::PerformanceEntry> new_entry)
{
    // 1. Let interested observers be an initially empty set of PerformanceObserver objects.
    Vector<JS::Handle<PerformanceTimeline::PerformanceObserver>> interested_observers;

    // 2. Let entryType be newEntry’s entryType value.
    auto const& entry_type = new_entry->entry_type();

    // 3. Let relevantGlobal be newEntry's relevant global object.
    // NOTE: Already is `this`.

    // 4. For each registered performance observer regObs in relevantGlobal's list of registered performance observer
    //    objects:
    for (auto const& registered_observer : m_registered_performance_observer_objects) {
        // 1. If regObs's options list contains a PerformanceObserverInit options whose entryTypes member includes entryType
        //    or whose type member equals to entryType:
        auto iterator = registered_observer->options_list().find_if([&entry_type](PerformanceTimeline::PerformanceObserverInit const& entry) {
            if (entry.entry_types.has_value())
                return entry.entry_types->contains_slow(entry_type.to_string());

            VERIFY(entry.type.has_value());
            return entry.type.value() == entry_type;
        });

        if (!iterator.is_end()) {
            // 1. If should add entry with newEntry and options returns true, append regObs's observer to interested observers.
            if (new_entry->should_add_entry(*iterator) == PerformanceTimeline::ShouldAddEntry::Yes)
                interested_observers.append(registered_observer);
        }
    }

    // 5. For each observer in interested observers:
    for (auto const& observer : interested_observers) {
        // 1. Append newEntry to observer's observer buffer.
        observer->append_to_observer_buffer({}, new_entry);
    }

    // 6. Let tuple be the relevant performance entry tuple of entryType and relevantGlobal.
    auto& tuple = relevant_performance_entry_tuple(entry_type);

    // 7. Let isBufferFull be the return value of the determine if a performance entry buffer is full algorithm with tuple
    //    as input.
    bool is_buffer_full = tuple.is_full();

    // 8. Let shouldAdd be the result of should add entry with newEntry as input.
    auto should_add = new_entry->should_add_entry();

    // 9. If isBufferFull is false and shouldAdd is true, append newEntry to tuple's performance entry buffer.
    if (!is_buffer_full && should_add == PerformanceTimeline::ShouldAddEntry::Yes)
        tuple.performance_entry_buffer.append(new_entry);

    // 10. Queue the PerformanceObserver task with relevantGlobal as input.
    queue_the_performance_observer_task();
}

void WindowOrWorkerGlobalScopeMixin::clear_performance_entry_buffer(Badge<HighResolutionTime::Performance>, FlyString const& entry_type)
{
    auto& tuple = relevant_performance_entry_tuple(entry_type);
    tuple.performance_entry_buffer.clear();
}

void WindowOrWorkerGlobalScopeMixin::remove_entries_from_performance_entry_buffer(Badge<HighResolutionTime::Performance>, FlyString const& entry_type, String entry_name)
{
    auto& tuple = relevant_performance_entry_tuple(entry_type);
    tuple.performance_entry_buffer.remove_all_matching([&entry_name](JS::Handle<PerformanceTimeline::PerformanceEntry> const& entry) {
        return entry->name() == entry_name;
    });
}

// https://www.w3.org/TR/performance-timeline/#dfn-filter-buffer-map-by-name-and-type
ErrorOr<Vector<JS::Handle<PerformanceTimeline::PerformanceEntry>>> WindowOrWorkerGlobalScopeMixin::filter_buffer_map_by_name_and_type(Optional<String> name, Optional<String> type) const
{
    // 1. Let result be an initially empty list.
    Vector<JS::Handle<PerformanceTimeline::PerformanceEntry>> result;

    // 2. Let map be the performance entry buffer map associated with the relevant global object of this.
    auto const& map = m_performance_entry_buffer_map;

    // 3. Let tuple list be an empty list.
    Vector<PerformanceTimeline::PerformanceEntryTuple const&> tuple_list;

    // 4. If type is not null, append the result of getting the value of entry on map given type as key to tuple list.
    //    Otherwise, assign the result of get the values on map to tuple list.
    if (type.has_value()) {
        auto maybe_tuple = map.get(type.value());
        if (maybe_tuple.has_value())
            TRY(tuple_list.try_append(maybe_tuple.release_value()));
    } else {
        for (auto const& it : map)
            TRY(tuple_list.try_append(it.value));
    }

    // 5. For each tuple in tuple list, run the following steps:
    for (auto const& tuple : tuple_list) {
        // 1. Let buffer be tuple's performance entry buffer.
        auto const& buffer = tuple.performance_entry_buffer;

        // 2. If tuple's availableFromTimeline is false, continue to the next tuple.
        if (tuple.available_from_timeline == PerformanceTimeline::AvailableFromTimeline::No)
            continue;

        // 3. Let entries be the result of running filter buffer by name and type with buffer, name and type as inputs.
        auto entries = TRY(filter_buffer_by_name_and_type(buffer, name, type));

        // 4. For each entry in entries, append entry to result.
        TRY(result.try_extend(entries));
    }

    // 6. Sort results's entries in chronological order with respect to startTime
    quick_sort(result, [](auto const& left_entry, auto const& right_entry) {
        return left_entry->start_time() < right_entry->start_time();
    });

    // 7. Return result.
    return result;
}

void WindowOrWorkerGlobalScopeMixin::register_performance_observer(Badge<PerformanceTimeline::PerformanceObserver>, JS::NonnullGCPtr<PerformanceTimeline::PerformanceObserver> observer)
{
    m_registered_performance_observer_objects.set(observer, AK::HashSetExistingEntryBehavior::Keep);
}

void WindowOrWorkerGlobalScopeMixin::unregister_performance_observer(Badge<PerformanceTimeline::PerformanceObserver>, JS::NonnullGCPtr<PerformanceTimeline::PerformanceObserver> observer)
{
    m_registered_performance_observer_objects.remove(observer);
}

bool WindowOrWorkerGlobalScopeMixin::has_registered_performance_observer(JS::NonnullGCPtr<PerformanceTimeline::PerformanceObserver> observer)
{
    return m_registered_performance_observer_objects.contains(observer);
}

// https://w3c.github.io/performance-timeline/#dfn-queue-the-performanceobserver-task
void WindowOrWorkerGlobalScopeMixin::queue_the_performance_observer_task()
{
    // 1. If relevantGlobal's performance observer task queued flag is set, terminate these steps.
    if (m_performance_observer_task_queued)
        return;

    // 2. Set relevantGlobal's performance observer task queued flag.
    m_performance_observer_task_queued = true;

    // 3. Queue a task that consists of running the following substeps. The task source for the queued task is the performance
    //    timeline task source.
    queue_global_task(Task::Source::PerformanceTimeline, this_impl(), JS::create_heap_function(this_impl().heap(), [this]() {
        auto& realm = this_impl().realm();

        // 1. Unset performance observer task queued flag of relevantGlobal.
        m_performance_observer_task_queued = false;

        // 2. Let notifyList be a copy of relevantGlobal's list of registered performance observer objects.
        auto notify_list = m_registered_performance_observer_objects;

        // 3. For each registered performance observer object registeredObserver in notifyList, run these steps:
        for (auto& registered_observer : notify_list) {
            // 1. Let po be registeredObserver's observer.
            // 2. Let entries be a copy of po’s observer buffer.
            // 4. Empty po’s observer buffer.
            auto entries = registered_observer->take_records();

            // 3. If entries is empty, return.
            // FIXME: Do they mean `continue`?
            if (entries.is_empty())
                continue;

            Vector<JS::NonnullGCPtr<PerformanceTimeline::PerformanceEntry>> entries_as_gc_ptrs;
            for (auto& entry : entries)
                entries_as_gc_ptrs.append(*entry);

            // 5. Let observerEntryList be a new PerformanceObserverEntryList, with its entry list set to entries.
            auto observer_entry_list = realm.heap().allocate<PerformanceTimeline::PerformanceObserverEntryList>(realm, realm, move(entries_as_gc_ptrs));

            // 6. Let droppedEntriesCount be null.
            Optional<u64> dropped_entries_count;

            // 7. If po's requires dropped entries is set, perform the following steps:
            if (registered_observer->requires_dropped_entries()) {
                // 1. Set droppedEntriesCount to 0.
                dropped_entries_count = 0;

                // 2. For each PerformanceObserverInit item in registeredObserver's options list:
                for (auto const& item : registered_observer->options_list()) {
                    // 1. For each DOMString entryType that appears either as item's type or in item's entryTypes:
                    auto increment_dropped_entries_count = [this, &dropped_entries_count](FlyString const& type) {
                        // 1. Let map be relevantGlobal's performance entry buffer map.
                        auto const& map = m_performance_entry_buffer_map;

                        // 2. Let tuple be the result of getting the value of entry on map given entryType as key.
                        auto const& tuple = map.get(type);
                        VERIFY(tuple.has_value());

                        // 3. Increase droppedEntriesCount by tuple's dropped entries count.
                        dropped_entries_count.value() += tuple->dropped_entries_count;
                    };

                    if (item.type.has_value()) {
                        increment_dropped_entries_count(item.type.value());
                    } else {
                        VERIFY(item.entry_types.has_value());
                        for (auto const& type : item.entry_types.value())
                            increment_dropped_entries_count(type);
                    }
                }

                // 3. Set po's requires dropped entries to false.
                registered_observer->unset_requires_dropped_entries({});
            }

            // 8. Let callbackOptions be a PerformanceObserverCallbackOptions with its droppedEntriesCount set to
            //    droppedEntriesCount if droppedEntriesCount is not null, otherwise unset.
            auto callback_options = JS::Object::create(realm, realm.intrinsics().object_prototype());
            if (dropped_entries_count.has_value())
                MUST(callback_options->create_data_property("droppedEntriesCount", JS::Value(dropped_entries_count.value())));

            // 9. Call po’s observer callback with observerEntryList as the first argument, with po as the second
            //    argument and as callback this value, and with callbackOptions as the third argument.
            //    If this throws an exception, report the exception.
            auto completion = WebIDL::invoke_callback(registered_observer->callback(), registered_observer, observer_entry_list, registered_observer, callback_options);
            if (completion.is_abrupt())
                HTML::report_exception(completion, realm);
        }
    }));
}

void WindowOrWorkerGlobalScopeMixin::register_event_source(Badge<EventSource>, JS::NonnullGCPtr<EventSource> event_source)
{
    m_registered_event_sources.set(event_source);
}

void WindowOrWorkerGlobalScopeMixin::unregister_event_source(Badge<EventSource>, JS::NonnullGCPtr<EventSource> event_source)
{
    m_registered_event_sources.remove(event_source);
}

void WindowOrWorkerGlobalScopeMixin::forcibly_close_all_event_sources()
{
    for (auto event_source : m_registered_event_sources)
        event_source->forcibly_close();
}

// https://html.spec.whatwg.org/multipage/timers-and-user-prompts.html#run-steps-after-a-timeout
void WindowOrWorkerGlobalScopeMixin::run_steps_after_a_timeout(i32 timeout, Function<void()> completion_step)
{
    return run_steps_after_a_timeout_impl(timeout, move(completion_step));
}

void WindowOrWorkerGlobalScopeMixin::run_steps_after_a_timeout_impl(i32 timeout, Function<void()> completion_step, Optional<i32> timer_key)
{
    // 1. Assert: if timerKey is given, then the caller of this algorithm is the timer initialization steps. (Other specifications must not pass timerKey.)
    // Note: This is enforced by the caller.

    // 2. If timerKey is not given, then set it to a new unique non-numeric value.
    if (!timer_key.has_value())
        timer_key = m_timer_id_allocator.allocate();

    // FIXME: 3. Let startTime be the current high resolution time given global.
    auto timer = Timer::create(this_impl(), timeout, move(completion_step), timer_key.value());

    // FIXME: 4. Set global's map of active timers[timerKey] to startTime plus milliseconds.
    m_timers.set(timer_key.value(), timer);

    // FIXME: 5. Run the following steps in parallel:
    // FIXME:    1. If global is a Window object, wait until global's associated Document has been fully active for a further milliseconds milliseconds (not necessarily consecutively).
    //              Otherwise, global is a WorkerGlobalScope object; wait until milliseconds milliseconds have passed with the worker not suspended (not necessarily consecutively).
    // FIXME:    2. Wait until any invocations of this algorithm that had the same global and orderingIdentifier, that started before this one, and whose milliseconds is equal to or less than this one's, have completed.
    // FIXME:    3. Optionally, wait a further implementation-defined length of time.
    // FIXME:    4. Perform completionSteps.
    // FIXME:    5. If timerKey is a non-numeric value, remove global's map of active timers[timerKey].

    timer->start();
}

// https://w3c.github.io/hr-time/#dom-windoworworkerglobalscope-performance
JS::NonnullGCPtr<HighResolutionTime::Performance> WindowOrWorkerGlobalScopeMixin::performance()
{
    auto& realm = this_impl().realm();
    if (!m_performance)
        m_performance = this_impl().heap().allocate<HighResolutionTime::Performance>(realm, realm);
    return JS::NonnullGCPtr { *m_performance };
}

JS::NonnullGCPtr<IndexedDB::IDBFactory> WindowOrWorkerGlobalScopeMixin::indexed_db()
{
    auto& vm = this_impl().vm();
    auto& realm = this_impl().realm();

    if (!m_indexed_db)
        m_indexed_db = vm.heap().allocate<IndexedDB::IDBFactory>(realm, realm);
    return *m_indexed_db;
}

// https://w3c.github.io/performance-timeline/#dfn-frozen-array-of-supported-entry-types
JS::NonnullGCPtr<JS::Object> WindowOrWorkerGlobalScopeMixin::supported_entry_types() const
{
    // Each global object has an associated frozen array of supported entry types, which is initialized to the
    // FrozenArray created from the sequence of strings among the registry that are supported for the global
    // object, in alphabetical order.
    auto& vm = this_impl().vm();
    auto& realm = this_impl().realm();

    if (!m_supported_entry_types_array) {
        Vector<JS::Value> supported_entry_types;

#define __ENUMERATE_SUPPORTED_PERFORMANCE_ENTRY_TYPES(entry_type, cpp_class) \
    supported_entry_types.append(JS::PrimitiveString::create(vm, entry_type));
        ENUMERATE_SUPPORTED_PERFORMANCE_ENTRY_TYPES
#undef __ENUMERATE_SUPPORTED_PERFORMANCE_ENTRY_TYPES

        m_supported_entry_types_array = JS::Array::create_from(realm, supported_entry_types);
        MUST(m_supported_entry_types_array->set_integrity_level(JS::Object::IntegrityLevel::Frozen));
    }

    return *m_supported_entry_types_array;
}

// https://html.spec.whatwg.org/multipage/webappapis.html#dom-reporterror
void WindowOrWorkerGlobalScopeMixin::report_error(JS::Value e)
{
    // The reportError(e) method steps are to report an exception e for this.
    report_an_exception(e);
}

// https://html.spec.whatwg.org/multipage/webappapis.html#report-an-exception
void WindowOrWorkerGlobalScopeMixin::report_an_exception(JS::Value const& e)
{
    auto& target = static_cast<DOM::EventTarget&>(this_impl());
    auto& realm = relevant_realm(target);
    auto& vm = realm.vm();
    auto script_or_module = vm.get_active_script_or_module();

    // FIXME: Get the current position in the script.
    auto line = 0;
    auto col = 0;

    // 1. If target is in error reporting mode, then return; the error is not handled.
    if (m_error_reporting_mode) {
        report_exception_to_console(e, realm, ErrorInPromise::No);
        return;
    }

    // 2. Let target be in error reporting mode.
    m_error_reporting_mode = true;

    // 3. Let message be an implementation-defined string describing the error in a helpful manner.
    auto message = [&] {
        if (e.is_object()) {
            auto& object = e.as_object();
            if (MUST(object.has_own_property(vm.names.message))) {
                auto message = object.get_without_side_effects(vm.names.message);
                return message.to_string_without_side_effects();
            }
        }

        return MUST(String::formatted("Uncaught exception: {}", e.to_string_without_side_effects()));
    }();

    // 4. Let errorValue be the value that represents the error: in the case of an uncaught exception,
    //    that would be the value that was thrown; in the case of a JavaScript error that would be an Error object
    //    If there is no corresponding value, then the null value must be used instead.
    auto error_value = e;

    // 5. Let urlString be the result of applying the URL serializer to the URL record that corresponds to the resource from which script was obtained.
    // NOTE: urlString is set below once we have determined whether we are dealing with a script or a module.
    String url_string;
    auto script_or_module_filename = [](auto const& script_or_module) {
        return MUST(String::from_utf8(script_or_module->filename()));
    };

    // 6. If script is a classic script and script's muted errors is true, then set message to "Script error.",
    //    urlString to the empty string, line and col to 0, and errorValue to null.
    script_or_module.visit(
        [&](JS::NonnullGCPtr<JS::Script> const& js_script) {
            if (verify_cast<ClassicScript>(js_script->host_defined())->muted_errors() == ClassicScript::MutedErrors::Yes) {
                message = "Script error."_string;
                url_string = String {};
                line = 0;
                col = 0;
                error_value = JS::js_null();
            } else {
                url_string = script_or_module_filename(js_script);
            }
        },
        [&](JS::NonnullGCPtr<JS::Module> const& js_module) {
            url_string = script_or_module_filename(js_module);
        },
        [](Empty) {});

    // 7. Let notHandled be true.
    auto not_handled = true;

    // 8. If target implements EventTarget, then set notHandled to the result of firing an event named error at target,
    //    using ErrorEvent, with the cancelable attribute initialized to true, the message attribute initialized to message,
    //    the filename attribute initialized to urlString, the lineno attribute initialized to line, the colno attribute initialized to col,
    //    and the error attribute initialized to errorValue.
    ErrorEventInit event_init = {};
    event_init.cancelable = true;
    event_init.message = message;
    event_init.filename = url_string;
    event_init.lineno = line;
    event_init.colno = col;
    event_init.error = error_value;

    not_handled = target.dispatch_event(ErrorEvent::create(realm, EventNames::error, event_init));

    // 9. Let target no longer be in error reporting mode.
    m_error_reporting_mode = false;

    // 10. If notHandled is false, then the error is handled. Otherwise, the error is not handled.
    if (not_handled) {
        // When the user agent is to report an exception E, the user agent must report the error for the relevant script,
        // with the problematic position (line number and column number) in the resource containing the script,
        // using the global object specified by the script's settings object as the target.
        // If the error is still not handled after this, then the error may be reported to a developer console.
        // https://html.spec.whatwg.org/multipage/webappapis.html#report-the-exception
        report_exception_to_console(e, realm, ErrorInPromise::No);
    }
}

// https://w3c.github.io/webcrypto/#dom-windoworworkerglobalscope-crypto
JS::NonnullGCPtr<Crypto::Crypto> WindowOrWorkerGlobalScopeMixin::crypto()
{
    auto& platform_object = this_impl();
    auto& realm = platform_object.realm();

    if (!m_crypto)
        m_crypto = platform_object.heap().allocate<Crypto::Crypto>(realm, realm);
    return JS::NonnullGCPtr { *m_crypto };
}

void WindowOrWorkerGlobalScopeMixin::push_onto_outstanding_rejected_promises_weak_set(JS::Promise* promise)
{
    m_outstanding_rejected_promises_weak_set.append(promise);
}

bool WindowOrWorkerGlobalScopeMixin::remove_from_outstanding_rejected_promises_weak_set(JS::Promise* promise)
{
    return m_outstanding_rejected_promises_weak_set.remove_first_matching([&](JS::Promise* promise_in_set) {
        return promise == promise_in_set;
    });
}

void WindowOrWorkerGlobalScopeMixin::push_onto_about_to_be_notified_rejected_promises_list(JS::NonnullGCPtr<JS::Promise> promise)
{
    m_about_to_be_notified_rejected_promises_list.append(JS::make_handle(promise));
}

bool WindowOrWorkerGlobalScopeMixin::remove_from_about_to_be_notified_rejected_promises_list(JS::NonnullGCPtr<JS::Promise> promise)
{
    return m_about_to_be_notified_rejected_promises_list.remove_first_matching([&](auto& promise_in_list) {
        return promise == promise_in_list;
    });
}

// https://html.spec.whatwg.org/multipage/webappapis.html#notify-about-rejected-promises
void WindowOrWorkerGlobalScopeMixin::notify_about_rejected_promises(Badge<EventLoop>)
{
    auto& realm = this_impl().realm();

    // 1. Let list be a copy of settings object's about-to-be-notified rejected promises list.
    auto list = m_about_to_be_notified_rejected_promises_list;

    // 2. If list is empty, return.
    if (list.is_empty())
        return;

    // 3. Clear settings object's about-to-be-notified rejected promises list.
    m_about_to_be_notified_rejected_promises_list.clear();

    // 4. Let global be settings object's global object.
    // We need this as an event target for the unhandledrejection event below
    auto& global = verify_cast<DOM::EventTarget>(this_impl());

    // 5. Queue a global task on the DOM manipulation task source given global to run the following substep:
    queue_global_task(Task::Source::DOMManipulation, global, JS::create_heap_function(realm.heap(), [this, &global, list = move(list)] {
        auto& realm = global.realm();

        // 1. For each promise p in list:
        for (auto const& promise : list) {

            // 1. If p's [[PromiseIsHandled]] internal slot is true, continue to the next iteration of the loop.
            if (promise->is_handled())
                continue;

            // 2. Let notHandled be the result of firing an event named unhandledrejection at global, using PromiseRejectionEvent, with the cancelable attribute initialized to true,
            //    the promise attribute initialized to p, and the reason attribute initialized to the value of p's [[PromiseResult]] internal slot.
            PromiseRejectionEventInit event_init {
                {
                    .bubbles = false,
                    .cancelable = true,
                    .composed = false,
                },
                // Sadly we can't use .promise and .reason here, as we can't use the designator on the initialization of DOM::EventInit above.
                /* .promise = */ JS::make_handle(*promise),
                /* .reason = */ promise->result(),
            };

            auto promise_rejection_event = PromiseRejectionEvent::create(realm, HTML::EventNames::unhandledrejection, event_init);

            bool not_handled = global.dispatch_event(*promise_rejection_event);

            // 3. If notHandled is false, then the promise rejection is handled. Otherwise, the promise rejection is not handled.

            // 4. If p's [[PromiseIsHandled]] internal slot is false, add p to settings object's outstanding rejected promises weak set.
            if (!promise->is_handled())
                m_outstanding_rejected_promises_weak_set.append(*promise);

            // This algorithm results in promise rejections being marked as handled or not handled. These concepts parallel handled and not handled script errors.
            // If a rejection is still not handled after this, then the rejection may be reported to a developer console.
            if (not_handled)
                HTML::report_exception_to_console(promise->result(), realm, ErrorInPromise::Yes);
        }
    }));
}

}
