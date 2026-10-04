// The Streams Standard (streams.spec.whatwg.org): ReadableStream with its
// default and byte controllers and both readers, WritableStream,
// TransformStream, the queuing strategies, and the Encoding Standard's
// TextEncoderStream and TextDecoderStream on top of them. Written in JS,
// as the spec's algorithms are written, and run as the engine's own code:
// to a page these are native (no source text, no stack frames).
//
// The script is one function of the realm's global and of the built-ins it
// works with; it defines the interfaces on the global and answers with what
// the engine itself needs: a byte stream over a body it already holds, and
// reading a stream to its end.
//
// It is run when a page first touches a stream, which may be long after
// the page's own scripts began. So nothing here reads a built-in off the
// global or off a prototype as the script loads: `primordials` holds each
// one as it was when the realm was made, kept apart from anything a page
// can reach, and a page replacing a global or a prototype method changes
// nothing here.
(function (global, primordials) {
"use strict";

const Promise_ = primordials.Promise;
const promiseThen = primordials.promiseThen;
const ReflectApply = primordials.ReflectApply;
const PromiseResolve = ReflectApply(primordials.functionBind, primordials.promiseResolve, [Promise_]);
const ObjectDefineProperty = primordials.ObjectDefineProperty;
const ObjectGetOwnPropertyNames = primordials.ObjectGetOwnPropertyNames;
const ObjectGetOwnPropertyDescriptor = primordials.ObjectGetOwnPropertyDescriptor;
const ObjectSetPrototypeOf = primordials.ObjectSetPrototypeOf;
const ObjectCreate = primordials.ObjectCreate;
const TypeError_ = primordials.TypeError;
const RangeError_ = primordials.RangeError;
const ArrayBuffer_ = primordials.ArrayBuffer;
const ArrayBufferIsView = primordials.ArrayBufferIsView;
const arrayBufferSlice = primordials.arrayBufferSlice;
const arrayBufferTransfer = primordials.arrayBufferTransfer;
const arrayBufferByteLength = primordials.arrayBufferByteLength;
const arrayBufferDetached = primordials.arrayBufferDetached;
const Uint8Array_ = primordials.Uint8Array;
const DataView_ = primordials.DataView;
const typedArrayName = primordials.typedArrayName;
const typedArrayBuffer = primordials.typedArrayBuffer;
const typedArrayByteOffset = primordials.typedArrayByteOffset;
const typedArrayByteLength = primordials.typedArrayByteLength;
const typedArrayLength = primordials.typedArrayLength;
const typedArraySet = primordials.typedArraySet;
const dataViewBuffer = primordials.dataViewBuffer;
const dataViewByteOffset = primordials.dataViewByteOffset;
const dataViewByteLength = primordials.dataViewByteLength;
const SymbolAsyncIterator = primordials.SymbolAsyncIterator;
const SymbolIterator = primordials.SymbolIterator;
const SymbolToStringTag = primordials.SymbolToStringTag;
const NumberIsNaN = primordials.NumberIsNaN;
const MathMin = primordials.MathMin;
const StringFromCharCode = primordials.StringFromCharCode;
const queueMicrotask_ = primordials.queueMicrotask;
const AbortController_ = primordials.AbortController;
const AbortSignal_ = primordials.AbortSignal;
// An interface attribute's getter as the realm was born with it, or an
// ordinary read where the attribute is not an accessor on the prototype.
const abortSignalAborted = AbortSignal_ ? primordials.abortSignalAborted || function () { return this.aborted; } : undefined;
const abortSignalReason = AbortSignal_ ? primordials.abortSignalReason || function () { return this.reason; } : undefined;
const abortControllerAbort = AbortController_ ? primordials.abortControllerAbort : undefined;
const abortControllerSignal = AbortController_ ? primordials.abortControllerSignal || function () { return this.signal; } : undefined;
const eventTargetAdd = primordials.eventTargetAdd;
const eventTargetRemove = primordials.eventTargetRemove;
const TextEncoder_ = primordials.TextEncoder;
const TextDecoder_ = primordials.TextDecoder;
const textEncoderEncode = TextEncoder_ ? primordials.textEncoderEncode : undefined;
const textDecoderDecode = TextDecoder_ ? primordials.textDecoderDecode : undefined;
const AsyncIteratorPrototype = primordials.AsyncIteratorPrototype;
// By name: Int8Array to BigUint64Array, and Float16Array where the engine has it.
const typedArrayConstructors = primordials.typedArrayConstructors;

// The one argument that makes an interface's constructor build the object
// for the algorithms without running the page-facing steps.
const INTERNAL = {};

// --- Promises ---------------------------------------------------------------------------

function newPromise()
{
    const record = { promise: undefined, resolve: undefined, reject: undefined, state: "pending" };
    record.promise = new Promise_((resolve, reject) => {
        record.resolve = (value) => {
            if (record.state === "pending")
                record.state = "fulfilled";
            resolve(value);
        };
        record.reject = (reason) => {
            if (record.state === "pending")
                record.state = "rejected";
            reject(reason);
        };
    });
    return record;
}

function resolvedPromise(value)
{
    return PromiseResolve(value);
}

function rejectedPromise(reason)
{
    return new Promise_((resolve, reject) => reject(reason));
}

function react(promise, onFulfilled, onRejected)
{
    return ReflectApply(promiseThen, promise, [onFulfilled, onRejected]);
}

function uponFulfillment(promise, steps)
{
    return react(promise, steps, undefined);
}

function uponRejection(promise, steps)
{
    return react(promise, undefined, steps);
}

function setHandled(promise)
{
    react(promise, undefined, () => {});
}

// Calls a page's method and gives a promise for its result: a throw is a
// rejection.
function promiseCall(method, thisArg, args)
{
    try {
        return resolvedPromise(ReflectApply(method, thisArg, args));
    } catch (error) {
        return rejectedPromise(error);
    }
}

// --- WebIDL conversions -----------------------------------------------------------------------

function dictionary(value, what)
{
    if (value === undefined || value === null)
        return {};
    if (typeof value !== "object" && typeof value !== "function")
        throw new TypeError_(`Failed to convert value to '${what}'.`);
    return value;
}

function callbackMember(dict, name, what)
{
    const value = dict[name];
    if (value === undefined)
        return undefined;
    if (typeof value !== "function")
        throw new TypeError_(`Failed to read the '${name}' property from '${what}': The provided value is not a function.`);
    return value;
}

function unsignedLongLongEnforced(value, what)
{
    let number = +value;
    if (NumberIsNaN(number) || number === Infinity || number === -Infinity)
        throw new TypeError_(`${what} is not a finite number.`);
    number = number < 0 ? -Math.floor(-number) : Math.floor(number);
    if (number < 0 || number > Number.MAX_SAFE_INTEGER)
        throw new TypeError_(`${what} is outside the accepted range.`);
    return number;
}

function isObject(value)
{
    return (typeof value === "object" && value !== null) || typeof value === "function";
}

// --- Buffers ----------------------------------------------------------------------------------

function isTypedArray(view)
{
    return ArrayBufferIsView(view) && ReflectApply(typedArrayName, view, []) !== undefined;
}

function viewBuffer(view)
{
    return isTypedArray(view) ? ReflectApply(typedArrayBuffer, view, []) : ReflectApply(dataViewBuffer, view, []);
}

function viewByteOffset(view)
{
    return isTypedArray(view) ? ReflectApply(typedArrayByteOffset, view, []) : ReflectApply(dataViewByteOffset, view, []);
}

function viewByteLength(view)
{
    return isTypedArray(view) ? ReflectApply(typedArrayByteLength, view, []) : ReflectApply(dataViewByteLength, view, []);
}

function bufferByteLength(buffer)
{
    return ReflectApply(arrayBufferByteLength, buffer, []);
}

function isDetached(buffer)
{
    return ReflectApply(arrayBufferDetached, buffer, []);
}

function transferArrayBuffer(buffer)
{
    return ReflectApply(arrayBufferTransfer, buffer, []);
}

function copyDataBlockBytes(destination, destinationOffset, source, sourceOffset, count)
{
    ReflectApply(typedArraySet, new Uint8Array_(destination, destinationOffset, count), [new Uint8Array_(source, sourceOffset, count)]);
}

function cloneAsUint8Array(view)
{
    const buffer = ReflectApply(arrayBufferSlice, viewBuffer(view), [viewByteOffset(view), viewByteOffset(view) + viewByteLength(view)]);
    return new Uint8Array_(buffer);
}

// --- Queues -----------------------------------------------------------------------------------

// A first-in first-out list that takes from its front without moving the
// rest: arrays of a few thousand entries, taken in turn.
class Queue {
    constructor()
    {
        this.front = [];
        this.cursor = 0;
        this.back = [];
    }
    get length() { return this.front.length - this.cursor + this.back.length; }
    push(value) { this.back.push(value); }
    peek()
    {
        if (this.cursor < this.front.length)
            return this.front[this.cursor];
        return this.back[0];
    }
    shift()
    {
        if (this.cursor >= this.front.length) {
            this.front = this.back;
            this.back = [];
            this.cursor = 0;
        }
        const value = this.front[this.cursor];
        this.front[this.cursor] = undefined;
        this.cursor++;
        return value;
    }
    *values()
    {
        for (let i = this.cursor; i < this.front.length; i++)
            yield this.front[i];
        for (const value of this.back)
            yield value;
    }
}

function resetQueue(container)
{
    container.queue = new Queue();
    container.queueTotalSize = 0;
}

function dequeueValue(container)
{
    const pair = container.queue.shift();
    container.queueTotalSize -= pair.size;
    if (container.queueTotalSize < 0)
        container.queueTotalSize = 0;
    return pair.value;
}

function enqueueValueWithSize(container, value, size)
{
    if (typeof size !== "number" || NumberIsNaN(size) || size < 0)
        throw new RangeError_("The size of a chunk must be a non-negative number.");
    if (size === Infinity)
        throw new RangeError_("The size of a chunk must be finite.");
    container.queue.push({ value, size });
    container.queueTotalSize += size;
}

function peekQueueValue(container)
{
    return container.queue.peek().value;
}

// --- Queuing strategies ---------------------------------------------------------------------

function extractHighWaterMark(strategy, defaultHWM)
{
    if (strategy.highWaterMark === undefined)
        return defaultHWM;
    const highWaterMark = +strategy.highWaterMark;
    if (NumberIsNaN(highWaterMark) || highWaterMark < 0)
        throw new RangeError_("The highWaterMark must be a non-negative number.");
    return highWaterMark;
}

function extractSizeAlgorithm(strategy)
{
    if (strategy.size === undefined)
        return () => 1;
    const size = strategy.size;
    return (chunk) => ReflectApply(size, undefined, [chunk]);
}

// highWaterMark is read before size, as the dictionary's members are
// read in order.
function readStrategy(value)
{
    const strategy = dictionary(value, "QueuingStrategy");
    const highWaterMark = strategy.highWaterMark;
    const size = callbackMember(strategy, "size", "QueuingStrategy");
    return { highWaterMark, size };
}

const byteLengthSize = {
    size(chunk) { return chunk.byteLength; },
}.size;
const countSize = {
    size() { return 1; },
}.size;

class ByteLengthQueuingStrategy {
    #s;
    constructor(init)
    {
        if (init === undefined || init === null || !isObject(init) || init.highWaterMark === undefined)
            throw new TypeError_("Failed to construct 'ByteLengthQueuingStrategy': required member highWaterMark is undefined.");
        this.#s = { highWaterMark: +init.highWaterMark };
    }
    get highWaterMark()
    {
        if (!(#s in this))
            throw new TypeError_("Illegal invocation");
        return this.#s.highWaterMark;
    }
    get size()
    {
        if (!(#s in this))
            throw new TypeError_("Illegal invocation");
        return byteLengthSize;
    }
}

class CountQueuingStrategy {
    #s;
    constructor(init)
    {
        if (init === undefined || init === null || !isObject(init) || init.highWaterMark === undefined)
            throw new TypeError_("Failed to construct 'CountQueuingStrategy': required member highWaterMark is undefined.");
        this.#s = { highWaterMark: +init.highWaterMark };
    }
    get highWaterMark()
    {
        if (!(#s in this))
            throw new TypeError_("Illegal invocation");
        return this.#s.highWaterMark;
    }
    get size()
    {
        if (!(#s in this))
            throw new TypeError_("Illegal invocation");
        return countSize;
    }
}

// --- ReadableStream --------------------------------------------------------------------------

let streamSlots; // the record behind a ReadableStream, or undefined for anything else
let defaultReaderSlots;
let byobReaderSlots;
let defaultControllerSlots;
let byteControllerSlots;
let byobRequestSlots;
let iteratorSlots;

function readableStreamRecord(object)
{
    return {
        object,
        state: "readable",
        reader: undefined,
        storedError: undefined,
        disturbed: false,
        controller: undefined,
        detached: false,
    };
}

class ReadableStream {
    #s;
    static { streamSlots = (o) => (isObject(o) && #s in o) ? o.#s : undefined; }

    constructor(underlyingSource = undefined, strategy = undefined)
    {
        if (underlyingSource === INTERNAL) {
            this.#s = readableStreamRecord(this);
            return;
        }
        const source = underlyingSource === undefined ? null : underlyingSource;
        const dict = dictionary(source, "UnderlyingSource");
        const autoAllocateChunkSize = dict.autoAllocateChunkSize;
        const cancel = callbackMember(dict, "cancel", "UnderlyingSource");
        const pull = callbackMember(dict, "pull", "UnderlyingSource");
        const start = callbackMember(dict, "start", "UnderlyingSource");
        let type = dict.type;
        if (type !== undefined) {
            type = `${type}`;
            if (type !== "bytes")
                throw new TypeError_(`Failed to read the 'type' property from 'UnderlyingSource': The provided value '${type}' is not a valid enum value of type ReadableStreamType.`);
        }
        const strategyDict = readStrategy(strategy);
        this.#s = readableStreamRecord(this);
        const sourceDict = {
            autoAllocateChunkSize: autoAllocateChunkSize === undefined ? undefined : unsignedLongLongEnforced(autoAllocateChunkSize, "autoAllocateChunkSize"),
            cancel, pull, start, type,
        };
        if (type === "bytes") {
            if (strategyDict.size !== undefined)
                throw new RangeError_("The strategy for a byte stream cannot have a size function.");
            const highWaterMark = extractHighWaterMark(strategyDict, 0);
            setUpReadableByteStreamControllerFromUnderlyingSource(this.#s, source, sourceDict, highWaterMark);
        } else {
            const sizeAlgorithm = extractSizeAlgorithm(strategyDict);
            const highWaterMark = extractHighWaterMark(strategyDict, 1);
            setUpReadableStreamDefaultControllerFromUnderlyingSource(this.#s, source, sourceDict, highWaterMark, sizeAlgorithm);
        }
    }

    static from(asyncIterable)
    {
        return readableStreamFromIterable(asyncIterable).object;
    }

    get locked()
    {
        const stream = streamSlots(this);
        if (!stream)
            throw new TypeError_("Illegal invocation");
        return isReadableStreamLocked(stream);
    }

    cancel(reason = undefined)
    {
        const stream = streamSlots(this);
        if (!stream)
            return rejectedPromise(new TypeError_("Illegal invocation"));
        if (isReadableStreamLocked(stream))
            return rejectedPromise(new TypeError_("Cannot cancel a stream that already has a reader"));
        return readableStreamCancel(stream, reason);
    }

    getReader(options = undefined)
    {
        const stream = streamSlots(this);
        if (!stream)
            throw new TypeError_("Illegal invocation");
        const dict = dictionary(options, "ReadableStreamGetReaderOptions");
        let mode = dict.mode;
        if (mode !== undefined) {
            mode = `${mode}`;
            if (mode !== "byob")
                throw new TypeError_(`Failed to read the 'mode' property from 'ReadableStreamGetReaderOptions': The provided value '${mode}' is not a valid enum value of type ReadableStreamReaderMode.`);
            return acquireReadableStreamBYOBReader(stream).object;
        }
        return acquireReadableStreamDefaultReader(stream).object;
    }

    pipeThrough(transform, options = undefined)
    {
        const stream = streamSlots(this);
        if (!stream)
            throw new TypeError_("Illegal invocation");
        const pair = dictionary(transform, "ReadableWritablePair");
        const readable = pair.readable;
        if (!streamSlots(readable))
            throw new TypeError_("Failed to read the 'readable' property from 'ReadableWritablePair': The provided value is not of type 'ReadableStream'.");
        const writable = pair.writable;
        if (!writableSlots(writable))
            throw new TypeError_("Failed to read the 'writable' property from 'ReadableWritablePair': The provided value is not of type 'WritableStream'.");
        const pipe = readPipeOptions(options);
        if (isReadableStreamLocked(stream))
            throw new TypeError_("ReadableStream.prototype.pipeThrough cannot be used on a locked ReadableStream");
        if (isWritableStreamLocked(writableSlots(writable)))
            throw new TypeError_("ReadableStream.prototype.pipeThrough cannot be used on a locked WritableStream");
        const promise = readableStreamPipeTo(stream, writableSlots(writable), pipe.preventClose, pipe.preventAbort, pipe.preventCancel, pipe.signal);
        setHandled(promise);
        return readable;
    }

    pipeTo(destination, options = undefined)
    {
        const stream = streamSlots(this);
        if (!stream)
            return rejectedPromise(new TypeError_("Illegal invocation"));
        const writable = writableSlots(destination);
        if (!writable)
            return rejectedPromise(new TypeError_("Failed to execute 'pipeTo' on 'ReadableStream': parameter 1 is not of type 'WritableStream'."));
        let pipe;
        try {
            pipe = readPipeOptions(options);
        } catch (error) {
            return rejectedPromise(error);
        }
        if (isReadableStreamLocked(stream))
            return rejectedPromise(new TypeError_("ReadableStream.prototype.pipeTo cannot be used on a locked ReadableStream"));
        if (isWritableStreamLocked(writable))
            return rejectedPromise(new TypeError_("ReadableStream.prototype.pipeTo cannot be used on a locked WritableStream"));
        return readableStreamPipeTo(stream, writable, pipe.preventClose, pipe.preventAbort, pipe.preventCancel, pipe.signal);
    }

    tee()
    {
        const stream = streamSlots(this);
        if (!stream)
            throw new TypeError_("Illegal invocation");
        const branches = readableStreamTee(stream, false);
        return [branches[0].object, branches[1].object];
    }

    values(options = undefined)
    {
        const stream = streamSlots(this);
        if (!stream)
            throw new TypeError_("Illegal invocation");
        const dict = dictionary(options, "ReadableStreamIteratorOptions");
        const preventCancel = !!dict.preventCancel;
        const reader = acquireReadableStreamDefaultReader(stream);
        return new ReadableStreamAsyncIterator(INTERNAL, reader, preventCancel);
    }
}

function readPipeOptions(options)
{
    const dict = dictionary(options, "StreamPipeOptions");
    const preventAbort = !!dict.preventAbort;
    const preventCancel = !!dict.preventCancel;
    const preventClose = !!dict.preventClose;
    const signal = dict.signal;
    if (signal !== undefined && !isAbortSignal(signal))
        throw new TypeError_("Failed to read the 'signal' property from 'StreamPipeOptions': Failed to convert value to 'AbortSignal'.");
    return { preventAbort, preventCancel, preventClose, signal };
}

function isAbortSignal(value)
{
    if (!abortSignalAborted || !isObject(value))
        return false;
    try {
        ReflectApply(abortSignalAborted, value, []);
        return true;
    } catch (error) {
        return false;
    }
}

function createReadableStream(startAlgorithm, pullAlgorithm, cancelAlgorithm, highWaterMark = 1, sizeAlgorithm = () => 1)
{
    const stream = streamSlots(new ReadableStream(INTERNAL));
    const controller = defaultControllerSlots(new ReadableStreamDefaultController(INTERNAL));
    setUpReadableStreamDefaultController(stream, controller, startAlgorithm, pullAlgorithm, cancelAlgorithm, highWaterMark, sizeAlgorithm);
    return stream;
}

function createReadableByteStream(startAlgorithm, pullAlgorithm, cancelAlgorithm)
{
    const stream = streamSlots(new ReadableStream(INTERNAL));
    const controller = byteControllerSlots(new ReadableByteStreamController(INTERNAL));
    setUpReadableByteStreamController(stream, controller, startAlgorithm, pullAlgorithm, cancelAlgorithm, 0, undefined);
    return stream;
}

function isReadableStreamLocked(stream)
{
    return stream.reader !== undefined;
}

function readableStreamFromIterable(asyncIterable)
{
    let iterator;
    let nextMethod;
    const asyncMethod = asyncIterable === undefined || asyncIterable === null ? undefined : asyncIterable[SymbolAsyncIterator];
    if (asyncMethod !== undefined && asyncMethod !== null) {
        if (typeof asyncMethod !== "function")
            throw new TypeError_("The object's Symbol.asyncIterator is not a function.");
        iterator = ReflectApply(asyncMethod, asyncIterable, []);
        if (!isObject(iterator))
            throw new TypeError_("The async iterator is not an object.");
        nextMethod = iterator.next;
    } else {
        const syncMethod = asyncIterable === undefined || asyncIterable === null ? undefined : asyncIterable[SymbolIterator];
        if (typeof syncMethod !== "function")
            throw new TypeError_("ReadableStream.from takes an iterable or an async iterable.");
        const syncIterator = ReflectApply(syncMethod, asyncIterable, []);
        if (!isObject(syncIterator))
            throw new TypeError_("The iterator is not an object.");
        const syncNext = syncIterator.next;
        // CreateAsyncFromSyncIterator: each result's value is awaited.
        iterator = {
            next() {
                let result;
                try {
                    result = ReflectApply(syncNext, syncIterator, []);
                } catch (error) {
                    return rejectedPromise(error);
                }
                if (!isObject(result))
                    return rejectedPromise(new TypeError_("The iterator result is not an object."));
                const done = !!result.done;
                return react(resolvedPromise(result.value), (value) => ({ value, done }), undefined);
            },
            return(value) {
                const method = syncIterator.return;
                if (method === undefined || method === null)
                    return resolvedPromise({ value, done: true });
                let result;
                try {
                    result = ReflectApply(method, syncIterator, [value]);
                } catch (error) {
                    return rejectedPromise(error);
                }
                if (!isObject(result))
                    return rejectedPromise(new TypeError_("The iterator result is not an object."));
                return react(resolvedPromise(result.value), (v) => ({ value: v, done: true }), undefined);
            },
        };
        nextMethod = iterator.next;
    }
    let stream;
    const startAlgorithm = () => undefined;
    const pullAlgorithm = () => {
        let nextResult;
        try {
            nextResult = ReflectApply(nextMethod, iterator, []);
        } catch (error) {
            return rejectedPromise(error);
        }
        return react(resolvedPromise(nextResult), (iterResult) => {
            if (!isObject(iterResult))
                throw new TypeError_("The promise returned by the iterator's next() must fulfill with an object.");
            if (iterResult.done) {
                readableStreamDefaultControllerClose(stream.controller);
            } else {
                readableStreamDefaultControllerEnqueue(stream.controller, iterResult.value);
            }
        }, undefined);
    };
    const cancelAlgorithm = (reason) => {
        let returnMethod;
        try {
            returnMethod = iterator.return;
        } catch (error) {
            return rejectedPromise(error);
        }
        if (returnMethod === undefined || returnMethod === null)
            return resolvedPromise(undefined);
        let returnResult;
        try {
            returnResult = ReflectApply(returnMethod, iterator, [reason]);
        } catch (error) {
            return rejectedPromise(error);
        }
        return react(resolvedPromise(returnResult), (result) => {
            if (!isObject(result))
                throw new TypeError_("The promise returned by the iterator's return() must fulfill with an object.");
            return undefined;
        }, undefined);
    };
    stream = createReadableStream(startAlgorithm, pullAlgorithm, cancelAlgorithm, 0);
    return stream;
}

function readableStreamCancel(stream, reason)
{
    stream.disturbed = true;
    if (stream.state === "closed")
        return resolvedPromise(undefined);
    if (stream.state === "errored")
        return rejectedPromise(stream.storedError);
    readableStreamClose(stream);
    const reader = stream.reader;
    if (reader !== undefined && reader.kind === "byob") {
        const readIntoRequests = reader.readIntoRequests;
        reader.readIntoRequests = new Queue();
        for (const request of readIntoRequests.values())
            request.closeSteps(undefined);
    }
    const sourceCancelPromise = stream.controller.cancelSteps(reason);
    return react(sourceCancelPromise, () => undefined, undefined);
}

function readableStreamClose(stream)
{
    stream.state = "closed";
    const reader = stream.reader;
    if (reader === undefined)
        return;
    reader.closedPromise.resolve(undefined);
    if (reader.kind === "default") {
        const readRequests = reader.readRequests;
        reader.readRequests = new Queue();
        for (const request of readRequests.values())
            request.closeSteps();
    }
}

function readableStreamError(stream, error)
{
    stream.state = "errored";
    stream.storedError = error;
    const reader = stream.reader;
    if (reader === undefined)
        return;
    reader.closedPromise.reject(error);
    setHandled(reader.closedPromise.promise);
    if (reader.kind === "default")
        readableStreamDefaultReaderErrorReadRequests(reader, error);
    else
        readableStreamBYOBReaderErrorReadIntoRequests(reader, error);
}

function readableStreamAddReadRequest(stream, readRequest)
{
    stream.reader.readRequests.push(readRequest);
}

function readableStreamAddReadIntoRequest(stream, readIntoRequest)
{
    stream.reader.readIntoRequests.push(readIntoRequest);
}

function readableStreamFulfillReadRequest(stream, chunk, done)
{
    const readRequest = stream.reader.readRequests.shift();
    if (done)
        readRequest.closeSteps();
    else
        readRequest.chunkSteps(chunk);
}

function readableStreamFulfillReadIntoRequest(stream, chunk, done)
{
    const readIntoRequest = stream.reader.readIntoRequests.shift();
    if (done)
        readIntoRequest.closeSteps(chunk);
    else
        readIntoRequest.chunkSteps(chunk);
}

function readableStreamGetNumReadRequests(stream)
{
    return stream.reader.readRequests.length;
}

function readableStreamGetNumReadIntoRequests(stream)
{
    return stream.reader.readIntoRequests.length;
}

function readableStreamHasDefaultReader(stream)
{
    return stream.reader !== undefined && stream.reader.kind === "default";
}

function readableStreamHasBYOBReader(stream)
{
    return stream.reader !== undefined && stream.reader.kind === "byob";
}

// --- Readers ---------------------------------------------------------------------------------

function readerRecord(object, kind)
{
    return { object, kind, stream: undefined, closedPromise: undefined, readRequests: new Queue(), readIntoRequests: new Queue() };
}

class ReadableStreamDefaultReader {
    #s;
    static { defaultReaderSlots = (o) => (isObject(o) && #s in o) ? o.#s : undefined; }

    constructor(stream)
    {
        this.#s = readerRecord(this, "default");
        if (stream === INTERNAL)
            return;
        const record = streamSlots(stream);
        if (!record)
            throw new TypeError_("Failed to construct 'ReadableStreamDefaultReader': parameter 1 is not of type 'ReadableStream'.");
        setUpReadableStreamDefaultReader(this.#s, record);
    }

    get closed()
    {
        const reader = defaultReaderSlots(this);
        if (!reader)
            return rejectedPromise(new TypeError_("Illegal invocation"));
        return reader.closedPromise.promise;
    }

    cancel(reason = undefined)
    {
        const reader = defaultReaderSlots(this);
        if (!reader)
            return rejectedPromise(new TypeError_("Illegal invocation"));
        if (reader.stream === undefined)
            return rejectedPromise(new TypeError_("This readable stream reader has been released and cannot be used to cancel its previous owner stream"));
        return readableStreamCancel(reader.stream, reason);
    }

    read()
    {
        const reader = defaultReaderSlots(this);
        if (!reader)
            return rejectedPromise(new TypeError_("Illegal invocation"));
        if (reader.stream === undefined)
            return rejectedPromise(new TypeError_("This readable stream reader has been released and cannot be used to read from its previous owner stream"));
        const promise = newPromise();
        readableStreamDefaultReaderRead(reader, {
            chunkSteps: (chunk) => promise.resolve({ value: chunk, done: false }),
            closeSteps: () => promise.resolve({ value: undefined, done: true }),
            errorSteps: (error) => promise.reject(error),
        });
        return promise.promise;
    }

    releaseLock()
    {
        const reader = defaultReaderSlots(this);
        if (!reader)
            throw new TypeError_("Illegal invocation");
        if (reader.stream === undefined)
            return;
        readableStreamDefaultReaderRelease(reader);
    }
}

class ReadableStreamBYOBReader {
    #s;
    static { byobReaderSlots = (o) => (isObject(o) && #s in o) ? o.#s : undefined; }

    constructor(stream)
    {
        this.#s = readerRecord(this, "byob");
        if (stream === INTERNAL)
            return;
        const record = streamSlots(stream);
        if (!record)
            throw new TypeError_("Failed to construct 'ReadableStreamBYOBReader': parameter 1 is not of type 'ReadableStream'.");
        setUpReadableStreamBYOBReader(this.#s, record);
    }

    get closed()
    {
        const reader = byobReaderSlots(this);
        if (!reader)
            return rejectedPromise(new TypeError_("Illegal invocation"));
        return reader.closedPromise.promise;
    }

    cancel(reason = undefined)
    {
        const reader = byobReaderSlots(this);
        if (!reader)
            return rejectedPromise(new TypeError_("Illegal invocation"));
        if (reader.stream === undefined)
            return rejectedPromise(new TypeError_("This readable stream reader has been released and cannot be used to cancel its previous owner stream"));
        return readableStreamCancel(reader.stream, reason);
    }

    read(view, options = undefined)
    {
        const reader = byobReaderSlots(this);
        if (!reader)
            return rejectedPromise(new TypeError_("Illegal invocation"));
        if (!ArrayBufferIsView(view))
            return rejectedPromise(new TypeError_("Failed to execute 'read' on 'ReadableStreamBYOBReader': parameter 1 is not of type 'ArrayBufferView'."));
        let min = 1;
        try {
            const dict = dictionary(options, "ReadableStreamBYOBReaderReadOptions");
            if (dict.min !== undefined)
                min = unsignedLongLongEnforced(dict.min, "min");
        } catch (error) {
            return rejectedPromise(error);
        }
        if (viewByteLength(view) === 0)
            return rejectedPromise(new TypeError_("This readable stream reader cannot be used to read as the view has byte length equal to 0"));
        const buffer = viewBuffer(view);
        if (bufferByteLength(buffer) === 0)
            return rejectedPromise(new TypeError_("This readable stream reader cannot be used to read as the viewed array buffer has a byte length equal to 0"));
        if (isDetached(buffer))
            return rejectedPromise(new TypeError_("This readable stream reader cannot be used to read as the viewed array buffer is detached"));
        if (min === 0)
            return rejectedPromise(new TypeError_("Options \"min\" cannot be 0"));
        if (isTypedArray(view)) {
            if (min > ReflectApply(typedArrayLength, view, []))
                return rejectedPromise(new RangeError_("The \"min\" option cannot be larger than the view's length"));
        } else if (min > viewByteLength(view)) {
            return rejectedPromise(new RangeError_("The \"min\" option cannot be larger than the view's byte length"));
        }
        if (reader.stream === undefined)
            return rejectedPromise(new TypeError_("This readable stream reader has been released and cannot be used to read from its previous owner stream"));
        const promise = newPromise();
        readableStreamBYOBReaderRead(reader, view, min, {
            chunkSteps: (chunk) => promise.resolve({ value: chunk, done: false }),
            closeSteps: (chunk) => promise.resolve({ value: chunk, done: true }),
            errorSteps: (error) => promise.reject(error),
        });
        return promise.promise;
    }

    releaseLock()
    {
        const reader = byobReaderSlots(this);
        if (!reader)
            throw new TypeError_("Illegal invocation");
        if (reader.stream === undefined)
            return;
        readableStreamBYOBReaderRelease(reader);
    }
}

function acquireReadableStreamDefaultReader(stream)
{
    const reader = defaultReaderSlots(new ReadableStreamDefaultReader(INTERNAL));
    setUpReadableStreamDefaultReader(reader, stream);
    return reader;
}

function acquireReadableStreamBYOBReader(stream)
{
    const reader = byobReaderSlots(new ReadableStreamBYOBReader(INTERNAL));
    setUpReadableStreamBYOBReader(reader, stream);
    return reader;
}

function readableStreamReaderGenericInitialize(reader, stream)
{
    reader.stream = stream;
    stream.reader = reader;
    reader.closedPromise = newPromise();
    if (stream.state === "closed") {
        reader.closedPromise.resolve(undefined);
    } else if (stream.state === "errored") {
        reader.closedPromise.reject(stream.storedError);
        setHandled(reader.closedPromise.promise);
    }
}

function readableStreamReaderGenericRelease(reader)
{
    const stream = reader.stream;
    const error = new TypeError_("Releasing reader");
    if (stream.state === "readable") {
        reader.closedPromise.reject(error);
    } else {
        reader.closedPromise = newPromise();
        reader.closedPromise.reject(error);
    }
    setHandled(reader.closedPromise.promise);
    stream.controller.releaseSteps();
    stream.reader = undefined;
    reader.stream = undefined;
}

function setUpReadableStreamDefaultReader(reader, stream)
{
    if (isReadableStreamLocked(stream))
        throw new TypeError_("ReadableStreamDefaultReader constructor can only accept readable streams that are not yet locked to a reader");
    readableStreamReaderGenericInitialize(reader, stream);
    reader.readRequests = new Queue();
}

function setUpReadableStreamBYOBReader(reader, stream)
{
    if (isReadableStreamLocked(stream))
        throw new TypeError_("ReadableStreamBYOBReader constructor can only accept readable streams that are not yet locked to a reader");
    if (stream.controller.kind !== "bytes")
        throw new TypeError_("Cannot construct a ReadableStreamBYOBReader for a stream not constructed with a byte source");
    readableStreamReaderGenericInitialize(reader, stream);
    reader.readIntoRequests = new Queue();
}

function readableStreamDefaultReaderRead(reader, readRequest)
{
    const stream = reader.stream;
    stream.disturbed = true;
    if (stream.state === "closed")
        readRequest.closeSteps();
    else if (stream.state === "errored")
        readRequest.errorSteps(stream.storedError);
    else
        stream.controller.pullSteps(readRequest);
}

function readableStreamDefaultReaderRelease(reader)
{
    readableStreamReaderGenericRelease(reader);
    readableStreamDefaultReaderErrorReadRequests(reader, new TypeError_("Releasing reader"));
}

function readableStreamDefaultReaderErrorReadRequests(reader, error)
{
    const readRequests = reader.readRequests;
    reader.readRequests = new Queue();
    for (const request of readRequests.values())
        request.errorSteps(error);
}

function readableStreamBYOBReaderRead(reader, view, min, readIntoRequest)
{
    const stream = reader.stream;
    stream.disturbed = true;
    if (stream.state === "errored")
        readIntoRequest.errorSteps(stream.storedError);
    else
        readableByteStreamControllerPullInto(stream.controller, view, min, readIntoRequest);
}

function readableStreamBYOBReaderRelease(reader)
{
    readableStreamReaderGenericRelease(reader);
    readableStreamBYOBReaderErrorReadIntoRequests(reader, new TypeError_("Releasing reader"));
}

function readableStreamBYOBReaderErrorReadIntoRequests(reader, error)
{
    const readIntoRequests = reader.readIntoRequests;
    reader.readIntoRequests = new Queue();
    for (const request of readIntoRequests.values())
        request.errorSteps(error);
}

// --- The default controller ---------------------------------------------------------------------

class ReadableStreamDefaultController {
    #s;
    static { defaultControllerSlots = (o) => (isObject(o) && #s in o) ? o.#s : undefined; }

    constructor(token = undefined)
    {
        if (token !== INTERNAL)
            throw new TypeError_("Illegal constructor");
        this.#s = {
            object: this,
            kind: "default",
            stream: undefined,
            queue: new Queue(),
            queueTotalSize: 0,
            started: false,
            closeRequested: false,
            pullAgain: false,
            pulling: false,
            strategyHWM: 0,
            strategySizeAlgorithm: undefined,
            pullAlgorithm: undefined,
            cancelAlgorithm: undefined,
            cancelSteps: undefined,
            pullSteps: undefined,
            releaseSteps: undefined,
        };
        const controller = this.#s;
        controller.cancelSteps = (reason) => {
            resetQueue(controller);
            const result = controller.cancelAlgorithm(reason);
            readableStreamDefaultControllerClearAlgorithms(controller);
            return result;
        };
        controller.pullSteps = (readRequest) => {
            const stream = controller.stream;
            if (controller.queue.length > 0) {
                const chunk = dequeueValue(controller);
                if (controller.closeRequested && controller.queue.length === 0) {
                    readableStreamDefaultControllerClearAlgorithms(controller);
                    readableStreamClose(stream);
                } else {
                    readableStreamDefaultControllerCallPullIfNeeded(controller);
                }
                readRequest.chunkSteps(chunk);
            } else {
                readableStreamAddReadRequest(stream, readRequest);
                readableStreamDefaultControllerCallPullIfNeeded(controller);
            }
        };
        controller.releaseSteps = () => {};
    }

    get desiredSize()
    {
        const controller = defaultControllerSlots(this);
        if (!controller)
            throw new TypeError_("Illegal invocation");
        return readableStreamDefaultControllerGetDesiredSize(controller);
    }

    close()
    {
        const controller = defaultControllerSlots(this);
        if (!controller)
            throw new TypeError_("Illegal invocation");
        if (!readableStreamDefaultControllerCanCloseOrEnqueue(controller))
            throw new TypeError_("The stream is not in a state that permits close");
        readableStreamDefaultControllerClose(controller);
    }

    enqueue(chunk = undefined)
    {
        const controller = defaultControllerSlots(this);
        if (!controller)
            throw new TypeError_("Illegal invocation");
        if (!readableStreamDefaultControllerCanCloseOrEnqueue(controller))
            throw new TypeError_("The stream is not in a state that permits enqueue");
        readableStreamDefaultControllerEnqueue(controller, chunk);
    }

    error(e = undefined)
    {
        const controller = defaultControllerSlots(this);
        if (!controller)
            throw new TypeError_("Illegal invocation");
        readableStreamDefaultControllerError(controller, e);
    }
}

function readableStreamDefaultControllerCallPullIfNeeded(controller)
{
    if (!readableStreamDefaultControllerShouldCallPull(controller))
        return;
    if (controller.pulling) {
        controller.pullAgain = true;
        return;
    }
    controller.pulling = true;
    const pullPromise = controller.pullAlgorithm();
    react(pullPromise, () => {
        controller.pulling = false;
        if (controller.pullAgain) {
            controller.pullAgain = false;
            readableStreamDefaultControllerCallPullIfNeeded(controller);
        }
    }, (error) => {
        readableStreamDefaultControllerError(controller, error);
    });
}

function readableStreamDefaultControllerShouldCallPull(controller)
{
    const stream = controller.stream;
    if (!readableStreamDefaultControllerCanCloseOrEnqueue(controller))
        return false;
    if (!controller.started)
        return false;
    if (isReadableStreamLocked(stream) && readableStreamGetNumReadRequests(stream) > 0)
        return true;
    return readableStreamDefaultControllerGetDesiredSize(controller) > 0;
}

function readableStreamDefaultControllerClearAlgorithms(controller)
{
    controller.pullAlgorithm = undefined;
    controller.cancelAlgorithm = undefined;
    controller.strategySizeAlgorithm = undefined;
}

function readableStreamDefaultControllerClose(controller)
{
    if (!readableStreamDefaultControllerCanCloseOrEnqueue(controller))
        return;
    controller.closeRequested = true;
    if (controller.queue.length === 0) {
        readableStreamDefaultControllerClearAlgorithms(controller);
        readableStreamClose(controller.stream);
    }
}

function readableStreamDefaultControllerEnqueue(controller, chunk)
{
    if (!readableStreamDefaultControllerCanCloseOrEnqueue(controller))
        return;
    const stream = controller.stream;
    if (isReadableStreamLocked(stream) && readableStreamGetNumReadRequests(stream) > 0) {
        readableStreamFulfillReadRequest(stream, chunk, false);
    } else {
        let chunkSize;
        try {
            chunkSize = controller.strategySizeAlgorithm(chunk);
        } catch (error) {
            readableStreamDefaultControllerError(controller, error);
            throw error;
        }
        try {
            enqueueValueWithSize(controller, chunk, chunkSize);
        } catch (error) {
            readableStreamDefaultControllerError(controller, error);
            throw error;
        }
    }
    readableStreamDefaultControllerCallPullIfNeeded(controller);
}

function readableStreamDefaultControllerError(controller, error)
{
    const stream = controller.stream;
    if (stream.state !== "readable")
        return;
    resetQueue(controller);
    readableStreamDefaultControllerClearAlgorithms(controller);
    readableStreamError(stream, error);
}

function readableStreamDefaultControllerGetDesiredSize(controller)
{
    const state = controller.stream.state;
    if (state === "errored")
        return null;
    if (state === "closed")
        return 0;
    return controller.strategyHWM - controller.queueTotalSize;
}

function readableStreamDefaultControllerHasBackpressure(controller)
{
    return !readableStreamDefaultControllerShouldCallPull(controller);
}

function readableStreamDefaultControllerCanCloseOrEnqueue(controller)
{
    return !controller.closeRequested && controller.stream.state === "readable";
}

function setUpReadableStreamDefaultController(stream, controller, startAlgorithm, pullAlgorithm, cancelAlgorithm, highWaterMark, sizeAlgorithm)
{
    controller.stream = stream;
    resetQueue(controller);
    controller.started = false;
    controller.closeRequested = false;
    controller.pullAgain = false;
    controller.pulling = false;
    controller.strategySizeAlgorithm = sizeAlgorithm;
    controller.strategyHWM = highWaterMark;
    controller.pullAlgorithm = pullAlgorithm;
    controller.cancelAlgorithm = cancelAlgorithm;
    stream.controller = controller;
    const startResult = startAlgorithm();
    react(resolvedPromise(startResult), () => {
        controller.started = true;
        readableStreamDefaultControllerCallPullIfNeeded(controller);
    }, (error) => {
        readableStreamDefaultControllerError(controller, error);
    });
}

function setUpReadableStreamDefaultControllerFromUnderlyingSource(stream, source, dict, highWaterMark, sizeAlgorithm)
{
    const controller = defaultControllerSlots(new ReadableStreamDefaultController(INTERNAL));
    let startAlgorithm = () => undefined;
    let pullAlgorithm = () => resolvedPromise(undefined);
    let cancelAlgorithm = () => resolvedPromise(undefined);
    if (dict.start !== undefined)
        startAlgorithm = () => ReflectApply(dict.start, source, [controller.object]);
    if (dict.pull !== undefined)
        pullAlgorithm = () => promiseCall(dict.pull, source, [controller.object]);
    if (dict.cancel !== undefined)
        cancelAlgorithm = (reason) => promiseCall(dict.cancel, source, [reason]);
    setUpReadableStreamDefaultController(stream, controller, startAlgorithm, pullAlgorithm, cancelAlgorithm, highWaterMark, sizeAlgorithm);
}

// --- The byte controller ---------------------------------------------------------------------------

class ReadableByteStreamController {
    #s;
    static { byteControllerSlots = (o) => (isObject(o) && #s in o) ? o.#s : undefined; }

    constructor(token = undefined)
    {
        if (token !== INTERNAL)
            throw new TypeError_("Illegal constructor");
        this.#s = {
            object: this,
            kind: "bytes",
            stream: undefined,
            autoAllocateChunkSize: undefined,
            byobRequest: null,
            queue: new Queue(),
            queueTotalSize: 0,
            started: false,
            closeRequested: false,
            pullAgain: false,
            pulling: false,
            strategyHWM: 0,
            pendingPullIntos: [],
            pullAlgorithm: undefined,
            cancelAlgorithm: undefined,
            cancelSteps: undefined,
            pullSteps: undefined,
            releaseSteps: undefined,
        };
        const controller = this.#s;
        controller.cancelSteps = (reason) => {
            readableByteStreamControllerClearPendingPullIntos(controller);
            resetQueue(controller);
            const result = controller.cancelAlgorithm(reason);
            readableByteStreamControllerClearAlgorithms(controller);
            return result;
        };
        controller.pullSteps = (readRequest) => {
            const stream = controller.stream;
            if (controller.queueTotalSize > 0) {
                readableByteStreamControllerFillReadRequestFromQueue(controller, readRequest);
                return;
            }
            const autoAllocateChunkSize = controller.autoAllocateChunkSize;
            if (autoAllocateChunkSize !== undefined) {
                let buffer;
                try {
                    buffer = new ArrayBuffer_(autoAllocateChunkSize);
                } catch (error) {
                    readRequest.errorSteps(error);
                    return;
                }
                controller.pendingPullIntos.push({
                    buffer,
                    bufferByteLength: autoAllocateChunkSize,
                    byteOffset: 0,
                    byteLength: autoAllocateChunkSize,
                    bytesFilled: 0,
                    minimumFill: 1,
                    elementSize: 1,
                    viewConstructor: Uint8Array_,
                    readerType: "default",
                });
            }
            readableStreamAddReadRequest(stream, readRequest);
            readableByteStreamControllerCallPullIfNeeded(controller);
        };
        controller.releaseSteps = () => {
            if (controller.pendingPullIntos.length > 0) {
                const first = controller.pendingPullIntos[0];
                first.readerType = "none";
                controller.pendingPullIntos = [first];
            }
        };
    }

    get byobRequest()
    {
        const controller = byteControllerSlots(this);
        if (!controller)
            throw new TypeError_("Illegal invocation");
        const request = readableByteStreamControllerGetBYOBRequest(controller);
        return request === null ? null : request.object;
    }

    get desiredSize()
    {
        const controller = byteControllerSlots(this);
        if (!controller)
            throw new TypeError_("Illegal invocation");
        return readableByteStreamControllerGetDesiredSize(controller);
    }

    close()
    {
        const controller = byteControllerSlots(this);
        if (!controller)
            throw new TypeError_("Illegal invocation");
        if (controller.closeRequested)
            throw new TypeError_("The stream has already been closed; do not close it again!");
        if (controller.stream.state !== "readable")
            throw new TypeError_("The stream is not in the readable state and cannot be closed");
        readableByteStreamControllerClose(controller);
    }

    enqueue(chunk)
    {
        const controller = byteControllerSlots(this);
        if (!controller)
            throw new TypeError_("Illegal invocation");
        if (!ArrayBufferIsView(chunk))
            throw new TypeError_("Failed to execute 'enqueue' on 'ReadableByteStreamController': parameter 1 is not of type 'ArrayBufferView'.");
        if (viewByteLength(chunk) === 0)
            throw new TypeError_("chunk must have non-zero byteLength");
        if (bufferByteLength(viewBuffer(chunk)) === 0)
            throw new TypeError_("chunk's buffer must have non-zero byteLength");
        if (controller.closeRequested)
            throw new TypeError_("stream is closed or draining");
        if (controller.stream.state !== "readable")
            throw new TypeError_("The stream is not in the readable state and cannot be enqueued to");
        readableByteStreamControllerEnqueue(controller, chunk);
    }

    error(e = undefined)
    {
        const controller = byteControllerSlots(this);
        if (!controller)
            throw new TypeError_("Illegal invocation");
        readableByteStreamControllerError(controller, e);
    }
}

class ReadableStreamBYOBRequest {
    #s;
    static { byobRequestSlots = (o) => (isObject(o) && #s in o) ? o.#s : undefined; }

    constructor(token = undefined)
    {
        if (token !== INTERNAL)
            throw new TypeError_("Illegal constructor");
        this.#s = { object: this, controller: undefined, view: null };
    }

    get view()
    {
        const request = byobRequestSlots(this);
        if (!request)
            throw new TypeError_("Illegal invocation");
        return request.view;
    }

    respond(bytesWritten)
    {
        const request = byobRequestSlots(this);
        if (!request)
            throw new TypeError_("Illegal invocation");
        const written = unsignedLongLongEnforced(bytesWritten, "bytesWritten");
        if (request.controller === undefined)
            throw new TypeError_("This BYOB request has been invalidated");
        if (isDetached(viewBuffer(request.view)))
            throw new TypeError_("The BYOB request's buffer has been detached and so cannot be used as a response");
        readableByteStreamControllerRespond(request.controller, written);
    }

    respondWithNewView(view)
    {
        const request = byobRequestSlots(this);
        if (!request)
            throw new TypeError_("Illegal invocation");
        if (!ArrayBufferIsView(view))
            throw new TypeError_("Failed to execute 'respondWithNewView' on 'ReadableStreamBYOBRequest': parameter 1 is not of type 'ArrayBufferView'.");
        if (request.controller === undefined)
            throw new TypeError_("This BYOB request has been invalidated");
        if (isDetached(viewBuffer(view)))
            throw new TypeError_("The given view's buffer has been detached and so cannot be used as a response");
        readableByteStreamControllerRespondWithNewView(request.controller, view);
    }
}

function readableByteStreamControllerCallPullIfNeeded(controller)
{
    if (!readableByteStreamControllerShouldCallPull(controller))
        return;
    if (controller.pulling) {
        controller.pullAgain = true;
        return;
    }
    controller.pulling = true;
    const pullPromise = controller.pullAlgorithm();
    react(pullPromise, () => {
        controller.pulling = false;
        if (controller.pullAgain) {
            controller.pullAgain = false;
            readableByteStreamControllerCallPullIfNeeded(controller);
        }
    }, (error) => {
        readableByteStreamControllerError(controller, error);
    });
}

function readableByteStreamControllerClearAlgorithms(controller)
{
    controller.pullAlgorithm = undefined;
    controller.cancelAlgorithm = undefined;
}

function readableByteStreamControllerClearPendingPullIntos(controller)
{
    readableByteStreamControllerInvalidateBYOBRequest(controller);
    controller.pendingPullIntos = [];
}

function readableByteStreamControllerClose(controller)
{
    const stream = controller.stream;
    if (controller.closeRequested || stream.state !== "readable")
        return;
    if (controller.queueTotalSize > 0) {
        controller.closeRequested = true;
        return;
    }
    if (controller.pendingPullIntos.length > 0) {
        const first = controller.pendingPullIntos[0];
        if (first.bytesFilled % first.elementSize !== 0) {
            const error = new TypeError_("Insufficient bytes to fill elements in the given buffer");
            readableByteStreamControllerError(controller, error);
            throw error;
        }
    }
    readableByteStreamControllerClearAlgorithms(controller);
    readableStreamClose(stream);
}

function readableByteStreamControllerCommitPullIntoDescriptor(stream, descriptor)
{
    let done = false;
    if (stream.state === "closed")
        done = true;
    const filledView = readableByteStreamControllerConvertPullIntoDescriptor(descriptor);
    if (descriptor.readerType === "default")
        readableStreamFulfillReadRequest(stream, filledView, done);
    else
        readableStreamFulfillReadIntoRequest(stream, filledView, done);
}

function readableByteStreamControllerConvertPullIntoDescriptor(descriptor)
{
    const bytesFilled = descriptor.bytesFilled;
    const elementSize = descriptor.elementSize;
    const buffer = transferArrayBuffer(descriptor.buffer);
    return new descriptor.viewConstructor(buffer, descriptor.byteOffset, bytesFilled / elementSize);
}

function readableByteStreamControllerEnqueue(controller, chunk)
{
    const stream = controller.stream;
    if (controller.closeRequested || stream.state !== "readable")
        return;
    const buffer = viewBuffer(chunk);
    const byteOffset = viewByteOffset(chunk);
    const byteLength = viewByteLength(chunk);
    if (isDetached(buffer))
        throw new TypeError_("chunk's buffer is detached and so cannot be enqueued");
    const transferredBuffer = transferArrayBuffer(buffer);
    if (controller.pendingPullIntos.length > 0) {
        const first = controller.pendingPullIntos[0];
        if (isDetached(first.buffer))
            throw new TypeError_("The BYOB request's buffer has been detached and so cannot be filled with an enqueued chunk");
        readableByteStreamControllerInvalidateBYOBRequest(controller);
        first.buffer = transferArrayBuffer(first.buffer);
        if (first.readerType === "none")
            readableByteStreamControllerEnqueueDetachedPullIntoToQueue(controller, first);
    }
    if (readableStreamHasDefaultReader(stream)) {
        readableByteStreamControllerProcessReadRequestsUsingQueue(controller);
        if (readableStreamGetNumReadRequests(stream) === 0) {
            readableByteStreamControllerEnqueueChunkToQueue(controller, transferredBuffer, byteOffset, byteLength);
        } else {
            if (controller.pendingPullIntos.length > 0)
                readableByteStreamControllerShiftPendingPullInto(controller);
            const transferredView = new Uint8Array_(transferredBuffer, byteOffset, byteLength);
            readableStreamFulfillReadRequest(stream, transferredView, false);
        }
    } else if (readableStreamHasBYOBReader(stream)) {
        readableByteStreamControllerEnqueueChunkToQueue(controller, transferredBuffer, byteOffset, byteLength);
        const filledPullIntos = readableByteStreamControllerProcessPullIntoDescriptorsUsingQueue(controller);
        for (const filled of filledPullIntos)
            readableByteStreamControllerCommitPullIntoDescriptor(stream, filled);
    } else {
        readableByteStreamControllerEnqueueChunkToQueue(controller, transferredBuffer, byteOffset, byteLength);
    }
    readableByteStreamControllerCallPullIfNeeded(controller);
}

function readableByteStreamControllerEnqueueChunkToQueue(controller, buffer, byteOffset, byteLength)
{
    controller.queue.push({ buffer, byteOffset, byteLength });
    controller.queueTotalSize += byteLength;
}

function readableByteStreamControllerEnqueueClonedChunkToQueue(controller, buffer, byteOffset, byteLength)
{
    let cloneResult;
    try {
        cloneResult = ReflectApply(arrayBufferSlice, buffer, [byteOffset, byteOffset + byteLength]);
    } catch (error) {
        readableByteStreamControllerError(controller, error);
        throw error;
    }
    readableByteStreamControllerEnqueueChunkToQueue(controller, cloneResult, 0, byteLength);
}

function readableByteStreamControllerEnqueueDetachedPullIntoToQueue(controller, descriptor)
{
    if (descriptor.bytesFilled > 0)
        readableByteStreamControllerEnqueueClonedChunkToQueue(controller, descriptor.buffer, descriptor.byteOffset, descriptor.bytesFilled);
    readableByteStreamControllerShiftPendingPullInto(controller);
}

function readableByteStreamControllerError(controller, error)
{
    const stream = controller.stream;
    if (stream.state !== "readable")
        return;
    readableByteStreamControllerClearPendingPullIntos(controller);
    resetQueue(controller);
    readableByteStreamControllerClearAlgorithms(controller);
    readableStreamError(stream, error);
}

function readableByteStreamControllerFillHeadPullIntoDescriptor(controller, size, descriptor)
{
    descriptor.bytesFilled += size;
}

function readableByteStreamControllerFillPullIntoDescriptorFromQueue(controller, descriptor)
{
    const maxBytesToCopy = MathMin(controller.queueTotalSize, descriptor.byteLength - descriptor.bytesFilled);
    const maxBytesFilled = descriptor.bytesFilled + maxBytesToCopy;
    let totalBytesToCopyRemaining = maxBytesToCopy;
    let ready = false;
    const remainderBytes = maxBytesFilled % descriptor.elementSize;
    const maxAlignedBytes = maxBytesFilled - remainderBytes;
    if (maxAlignedBytes >= descriptor.minimumFill) {
        totalBytesToCopyRemaining = maxAlignedBytes - descriptor.bytesFilled;
        ready = true;
    }
    const queue = controller.queue;
    while (totalBytesToCopyRemaining > 0) {
        const head = queue.peek();
        const bytesToCopy = MathMin(totalBytesToCopyRemaining, head.byteLength);
        const destinationStart = descriptor.byteOffset + descriptor.bytesFilled;
        copyDataBlockBytes(descriptor.buffer, destinationStart, head.buffer, head.byteOffset, bytesToCopy);
        if (head.byteLength === bytesToCopy) {
            queue.shift();
        } else {
            head.byteOffset += bytesToCopy;
            head.byteLength -= bytesToCopy;
        }
        controller.queueTotalSize -= bytesToCopy;
        readableByteStreamControllerFillHeadPullIntoDescriptor(controller, bytesToCopy, descriptor);
        totalBytesToCopyRemaining -= bytesToCopy;
    }
    return ready;
}

function readableByteStreamControllerFillReadRequestFromQueue(controller, readRequest)
{
    const entry = controller.queue.shift();
    controller.queueTotalSize -= entry.byteLength;
    readableByteStreamControllerHandleQueueDrain(controller);
    const view = new Uint8Array_(entry.buffer, entry.byteOffset, entry.byteLength);
    readRequest.chunkSteps(view);
}

function readableByteStreamControllerGetBYOBRequest(controller)
{
    if (controller.byobRequest === null && controller.pendingPullIntos.length > 0) {
        const first = controller.pendingPullIntos[0];
        const view = new Uint8Array_(first.buffer, first.byteOffset + first.bytesFilled, first.byteLength - first.bytesFilled);
        const request = byobRequestSlots(new ReadableStreamBYOBRequest(INTERNAL));
        request.controller = controller;
        request.view = view;
        controller.byobRequest = request;
    }
    return controller.byobRequest;
}

function readableByteStreamControllerGetDesiredSize(controller)
{
    const state = controller.stream.state;
    if (state === "errored")
        return null;
    if (state === "closed")
        return 0;
    return controller.strategyHWM - controller.queueTotalSize;
}

function readableByteStreamControllerHandleQueueDrain(controller)
{
    if (controller.queueTotalSize === 0 && controller.closeRequested) {
        readableByteStreamControllerClearAlgorithms(controller);
        readableStreamClose(controller.stream);
    } else {
        readableByteStreamControllerCallPullIfNeeded(controller);
    }
}

function readableByteStreamControllerInvalidateBYOBRequest(controller)
{
    if (controller.byobRequest === null)
        return;
    controller.byobRequest.controller = undefined;
    controller.byobRequest.view = null;
    controller.byobRequest = null;
}

function readableByteStreamControllerProcessPullIntoDescriptorsUsingQueue(controller)
{
    const filledPullIntos = [];
    while (controller.pendingPullIntos.length > 0) {
        if (controller.queueTotalSize === 0)
            break;
        const descriptor = controller.pendingPullIntos[0];
        if (readableByteStreamControllerFillPullIntoDescriptorFromQueue(controller, descriptor)) {
            readableByteStreamControllerShiftPendingPullInto(controller);
            filledPullIntos.push(descriptor);
        }
    }
    return filledPullIntos;
}

function readableByteStreamControllerProcessReadRequestsUsingQueue(controller)
{
    const reader = controller.stream.reader;
    while (reader.readRequests.length > 0) {
        if (controller.queueTotalSize === 0)
            return;
        const readRequest = reader.readRequests.shift();
        readableByteStreamControllerFillReadRequestFromQueue(controller, readRequest);
    }
}

function readableByteStreamControllerPullInto(controller, view, min, readIntoRequest)
{
    const stream = controller.stream;
    let elementSize = 1;
    let viewConstructor = DataView_;
    if (isTypedArray(view)) {
        const name = ReflectApply(typedArrayName, view, []);
        viewConstructor = typedArrayConstructors[name];
        elementSize = viewConstructor.BYTES_PER_ELEMENT;
    }
    const minimumFill = min * elementSize;
    const byteOffset = viewByteOffset(view);
    const byteLength = viewByteLength(view);
    let bufferResult;
    try {
        bufferResult = transferArrayBuffer(viewBuffer(view));
    } catch (error) {
        readIntoRequest.errorSteps(error);
        return;
    }
    const descriptor = {
        buffer: bufferResult,
        bufferByteLength: bufferByteLength(bufferResult),
        byteOffset,
        byteLength,
        bytesFilled: 0,
        minimumFill,
        elementSize,
        viewConstructor,
        readerType: "byob",
    };
    if (controller.pendingPullIntos.length > 0) {
        controller.pendingPullIntos.push(descriptor);
        readableStreamAddReadIntoRequest(stream, readIntoRequest);
        return;
    }
    if (stream.state === "closed") {
        const emptyView = new viewConstructor(descriptor.buffer, descriptor.byteOffset, 0);
        readIntoRequest.closeSteps(emptyView);
        return;
    }
    if (controller.queueTotalSize > 0) {
        if (readableByteStreamControllerFillPullIntoDescriptorFromQueue(controller, descriptor)) {
            const filledView = readableByteStreamControllerConvertPullIntoDescriptor(descriptor);
            readableByteStreamControllerHandleQueueDrain(controller);
            readIntoRequest.chunkSteps(filledView);
            return;
        }
        if (controller.closeRequested) {
            const error = new TypeError_("Insufficient bytes to fill elements in the given buffer");
            readableByteStreamControllerError(controller, error);
            readIntoRequest.errorSteps(error);
            return;
        }
    }
    controller.pendingPullIntos.push(descriptor);
    readableStreamAddReadIntoRequest(stream, readIntoRequest);
    readableByteStreamControllerCallPullIfNeeded(controller);
}

function readableByteStreamControllerRespond(controller, bytesWritten)
{
    const first = controller.pendingPullIntos[0];
    const state = controller.stream.state;
    if (state === "closed") {
        if (bytesWritten !== 0)
            throw new TypeError_("bytesWritten must be 0 when calling respond() on a closed stream");
    } else {
        if (bytesWritten === 0)
            throw new TypeError_("bytesWritten must be greater than 0 when calling respond() on a readable stream");
        if (first.bytesFilled + bytesWritten > first.byteLength)
            throw new RangeError_("bytesWritten out of range");
    }
    first.buffer = transferArrayBuffer(first.buffer);
    readableByteStreamControllerRespondInternal(controller, bytesWritten);
}

function readableByteStreamControllerRespondInClosedState(controller, first)
{
    if (first.readerType === "none")
        readableByteStreamControllerShiftPendingPullInto(controller);
    const stream = controller.stream;
    if (readableStreamHasBYOBReader(stream)) {
        const filledPullIntos = [];
        while (filledPullIntos.length < readableStreamGetNumReadIntoRequests(stream))
            filledPullIntos.push(readableByteStreamControllerShiftPendingPullInto(controller));
        for (const filled of filledPullIntos)
            readableByteStreamControllerCommitPullIntoDescriptor(stream, filled);
    }
}

function readableByteStreamControllerRespondInReadableState(controller, bytesWritten, descriptor)
{
    readableByteStreamControllerFillHeadPullIntoDescriptor(controller, bytesWritten, descriptor);
    if (descriptor.readerType === "none") {
        readableByteStreamControllerEnqueueDetachedPullIntoToQueue(controller, descriptor);
        const filledPullIntos = readableByteStreamControllerProcessPullIntoDescriptorsUsingQueue(controller);
        for (const filled of filledPullIntos)
            readableByteStreamControllerCommitPullIntoDescriptor(controller.stream, filled);
        return;
    }
    if (descriptor.bytesFilled < descriptor.minimumFill)
        return;
    readableByteStreamControllerShiftPendingPullInto(controller);
    const remainderSize = descriptor.bytesFilled % descriptor.elementSize;
    if (remainderSize > 0) {
        const end = descriptor.byteOffset + descriptor.bytesFilled;
        readableByteStreamControllerEnqueueClonedChunkToQueue(controller, descriptor.buffer, end - remainderSize, remainderSize);
    }
    descriptor.bytesFilled -= remainderSize;
    const filledPullIntos = readableByteStreamControllerProcessPullIntoDescriptorsUsingQueue(controller);
    readableByteStreamControllerCommitPullIntoDescriptor(controller.stream, descriptor);
    for (const filled of filledPullIntos)
        readableByteStreamControllerCommitPullIntoDescriptor(controller.stream, filled);
}

function readableByteStreamControllerRespondInternal(controller, bytesWritten)
{
    const first = controller.pendingPullIntos[0];
    readableByteStreamControllerInvalidateBYOBRequest(controller);
    if (controller.stream.state === "closed")
        readableByteStreamControllerRespondInClosedState(controller, first);
    else
        readableByteStreamControllerRespondInReadableState(controller, bytesWritten, first);
    readableByteStreamControllerCallPullIfNeeded(controller);
}

function readableByteStreamControllerRespondWithNewView(controller, view)
{
    const first = controller.pendingPullIntos[0];
    const state = controller.stream.state;
    const byteLength = viewByteLength(view);
    if (state === "closed") {
        if (byteLength !== 0)
            throw new TypeError_("The view's length must be 0 when calling respondWithNewView() on a closed stream");
    } else if (byteLength === 0) {
        throw new TypeError_("The view's length must be greater than 0 when calling respondWithNewView() on a readable stream");
    }
    if (first.byteOffset + first.bytesFilled !== viewByteOffset(view))
        throw new RangeError_("The region specified by view does not match byobRequest");
    if (first.bufferByteLength !== bufferByteLength(viewBuffer(view)))
        throw new RangeError_("The buffer of view has different capacity than byobRequest");
    if (first.bytesFilled + byteLength > first.byteLength)
        throw new RangeError_("The region specified by view is larger than byobRequest");
    first.buffer = transferArrayBuffer(viewBuffer(view));
    readableByteStreamControllerRespondInternal(controller, byteLength);
}

function readableByteStreamControllerShiftPendingPullInto(controller)
{
    return controller.pendingPullIntos.shift();
}

function readableByteStreamControllerShouldCallPull(controller)
{
    const stream = controller.stream;
    if (stream.state !== "readable")
        return false;
    if (controller.closeRequested)
        return false;
    if (!controller.started)
        return false;
    if (readableStreamHasDefaultReader(stream) && readableStreamGetNumReadRequests(stream) > 0)
        return true;
    if (readableStreamHasBYOBReader(stream) && readableStreamGetNumReadIntoRequests(stream) > 0)
        return true;
    return readableByteStreamControllerGetDesiredSize(controller) > 0;
}

function setUpReadableByteStreamController(stream, controller, startAlgorithm, pullAlgorithm, cancelAlgorithm, highWaterMark, autoAllocateChunkSize)
{
    controller.stream = stream;
    controller.pullAgain = false;
    controller.pulling = false;
    controller.byobRequest = null;
    resetQueue(controller);
    controller.closeRequested = false;
    controller.started = false;
    controller.strategyHWM = highWaterMark;
    controller.pullAlgorithm = pullAlgorithm;
    controller.cancelAlgorithm = cancelAlgorithm;
    controller.autoAllocateChunkSize = autoAllocateChunkSize;
    controller.pendingPullIntos = [];
    stream.controller = controller;
    const startResult = startAlgorithm();
    react(resolvedPromise(startResult), () => {
        controller.started = true;
        readableByteStreamControllerCallPullIfNeeded(controller);
    }, (error) => {
        readableByteStreamControllerError(controller, error);
    });
}

function setUpReadableByteStreamControllerFromUnderlyingSource(stream, source, dict, highWaterMark)
{
    const controller = byteControllerSlots(new ReadableByteStreamController(INTERNAL));
    let startAlgorithm = () => undefined;
    let pullAlgorithm = () => resolvedPromise(undefined);
    let cancelAlgorithm = () => resolvedPromise(undefined);
    if (dict.start !== undefined)
        startAlgorithm = () => ReflectApply(dict.start, source, [controller.object]);
    if (dict.pull !== undefined)
        pullAlgorithm = () => promiseCall(dict.pull, source, [controller.object]);
    if (dict.cancel !== undefined)
        cancelAlgorithm = (reason) => promiseCall(dict.cancel, source, [reason]);
    const autoAllocateChunkSize = dict.autoAllocateChunkSize;
    if (autoAllocateChunkSize === 0)
        throw new TypeError_("autoAllocateChunkSize must be greater than 0");
    setUpReadableByteStreamController(stream, controller, startAlgorithm, pullAlgorithm, cancelAlgorithm, highWaterMark, autoAllocateChunkSize);
}

// --- Tee ----------------------------------------------------------------------------------------

function readableStreamTee(stream, cloneForBranch2)
{
    if (stream.controller.kind === "bytes")
        return readableByteStreamTee(stream);
    return readableStreamDefaultTee(stream, cloneForBranch2);
}

function readableStreamDefaultTee(stream, cloneForBranch2)
{
    void cloneForBranch2;
    const reader = acquireReadableStreamDefaultReader(stream);
    let reading = false;
    let readAgain = false;
    let canceled1 = false;
    let canceled2 = false;
    let reason1;
    let reason2;
    let branch1;
    let branch2;
    const cancelPromise = newPromise();
    const pullAlgorithm = () => {
        if (reading) {
            readAgain = true;
            return resolvedPromise(undefined);
        }
        reading = true;
        readableStreamDefaultReaderRead(reader, {
            chunkSteps: (chunk) => {
                ReflectApply(queueMicrotask_, global, [() => {
                    readAgain = false;
                    if (!canceled1)
                        readableStreamDefaultControllerEnqueue(branch1.controller, chunk);
                    if (!canceled2)
                        readableStreamDefaultControllerEnqueue(branch2.controller, chunk);
                    reading = false;
                    if (readAgain)
                        pullAlgorithm();
                }]);
            },
            closeSteps: () => {
                reading = false;
                if (!canceled1)
                    readableStreamDefaultControllerClose(branch1.controller);
                if (!canceled2)
                    readableStreamDefaultControllerClose(branch2.controller);
                if (!canceled1 || !canceled2)
                    cancelPromise.resolve(undefined);
            },
            errorSteps: () => {
                reading = false;
            },
        });
        return resolvedPromise(undefined);
    };
    const cancel1Algorithm = (reason) => {
        canceled1 = true;
        reason1 = reason;
        if (canceled2)
            cancelPromise.resolve(readableStreamCancel(stream, [reason1, reason2]));
        return cancelPromise.promise;
    };
    const cancel2Algorithm = (reason) => {
        canceled2 = true;
        reason2 = reason;
        if (canceled1)
            cancelPromise.resolve(readableStreamCancel(stream, [reason1, reason2]));
        return cancelPromise.promise;
    };
    const startAlgorithm = () => undefined;
    branch1 = createReadableStream(startAlgorithm, pullAlgorithm, cancel1Algorithm);
    branch2 = createReadableStream(startAlgorithm, pullAlgorithm, cancel2Algorithm);
    uponRejection(reader.closedPromise.promise, (error) => {
        readableStreamDefaultControllerError(branch1.controller, error);
        readableStreamDefaultControllerError(branch2.controller, error);
        if (!canceled1 || !canceled2)
            cancelPromise.resolve(undefined);
    });
    return [branch1, branch2];
}

function readableByteStreamTee(stream)
{
    let reader = acquireReadableStreamDefaultReader(stream);
    let reading = false;
    let readAgainForBranch1 = false;
    let readAgainForBranch2 = false;
    let canceled1 = false;
    let canceled2 = false;
    let reason1;
    let reason2;
    let branch1;
    let branch2;
    const cancelPromise = newPromise();
    const forwardReaderError = (thisReader) => {
        uponRejection(thisReader.closedPromise.promise, (error) => {
            if (thisReader !== reader)
                return;
            readableByteStreamControllerError(branch1.controller, error);
            readableByteStreamControllerError(branch2.controller, error);
            if (!canceled1 || !canceled2)
                cancelPromise.resolve(undefined);
        });
    };
    const pullWithDefaultReader = () => {
        if (reader.kind === "byob") {
            readableStreamBYOBReaderRelease(reader);
            reader = acquireReadableStreamDefaultReader(stream);
            forwardReaderError(reader);
        }
        readableStreamDefaultReaderRead(reader, {
            chunkSteps: (chunk) => {
                ReflectApply(queueMicrotask_, global, [() => {
                    readAgainForBranch1 = false;
                    readAgainForBranch2 = false;
                    const chunk1 = chunk;
                    let chunk2 = chunk;
                    if (!canceled1 && !canceled2) {
                        try {
                            chunk2 = cloneAsUint8Array(chunk);
                        } catch (cloneError) {
                            readableByteStreamControllerError(branch1.controller, cloneError);
                            readableByteStreamControllerError(branch2.controller, cloneError);
                            cancelPromise.resolve(readableStreamCancel(stream, cloneError));
                            return;
                        }
                    }
                    if (!canceled1)
                        readableByteStreamControllerEnqueue(branch1.controller, chunk1);
                    if (!canceled2)
                        readableByteStreamControllerEnqueue(branch2.controller, chunk2);
                    reading = false;
                    if (readAgainForBranch1)
                        pull1Algorithm();
                    else if (readAgainForBranch2)
                        pull2Algorithm();
                }]);
            },
            closeSteps: () => {
                reading = false;
                if (!canceled1)
                    readableByteStreamControllerClose(branch1.controller);
                if (!canceled2)
                    readableByteStreamControllerClose(branch2.controller);
                if (branch1.controller.pendingPullIntos.length > 0)
                    readableByteStreamControllerRespond(branch1.controller, 0);
                if (branch2.controller.pendingPullIntos.length > 0)
                    readableByteStreamControllerRespond(branch2.controller, 0);
                if (!canceled1 || !canceled2)
                    cancelPromise.resolve(undefined);
            },
            errorSteps: () => {
                reading = false;
            },
        });
    };
    const pullWithBYOBReader = (view, forBranch2) => {
        if (reader.kind === "default") {
            readableStreamDefaultReaderRelease(reader);
            reader = acquireReadableStreamBYOBReader(stream);
            forwardReaderError(reader);
        }
        const byobBranch = forBranch2 ? branch2 : branch1;
        const otherBranch = forBranch2 ? branch1 : branch2;
        readableStreamBYOBReaderRead(reader, view, 1, {
            chunkSteps: (chunk) => {
                ReflectApply(queueMicrotask_, global, [() => {
                    readAgainForBranch1 = false;
                    readAgainForBranch2 = false;
                    const byobCanceled = forBranch2 ? canceled2 : canceled1;
                    const otherCanceled = forBranch2 ? canceled1 : canceled2;
                    if (!otherCanceled) {
                        let clonedChunk;
                        try {
                            clonedChunk = cloneAsUint8Array(chunk);
                        } catch (cloneError) {
                            readableByteStreamControllerError(byobBranch.controller, cloneError);
                            readableByteStreamControllerError(otherBranch.controller, cloneError);
                            cancelPromise.resolve(readableStreamCancel(stream, cloneError));
                            return;
                        }
                        if (!byobCanceled)
                            readableByteStreamControllerRespondWithNewView(byobBranch.controller, chunk);
                        readableByteStreamControllerEnqueue(otherBranch.controller, clonedChunk);
                    } else if (!byobCanceled) {
                        readableByteStreamControllerRespondWithNewView(byobBranch.controller, chunk);
                    }
                    reading = false;
                    if (readAgainForBranch1)
                        pull1Algorithm();
                    else if (readAgainForBranch2)
                        pull2Algorithm();
                }]);
            },
            closeSteps: (chunk) => {
                reading = false;
                const byobCanceled = forBranch2 ? canceled2 : canceled1;
                const otherCanceled = forBranch2 ? canceled1 : canceled2;
                if (!byobCanceled)
                    readableByteStreamControllerClose(byobBranch.controller);
                if (!otherCanceled)
                    readableByteStreamControllerClose(otherBranch.controller);
                if (chunk !== undefined) {
                    if (!byobCanceled)
                        readableByteStreamControllerRespondWithNewView(byobBranch.controller, chunk);
                    if (!otherCanceled && otherBranch.controller.pendingPullIntos.length > 0)
                        readableByteStreamControllerRespond(otherBranch.controller, 0);
                }
                if (!byobCanceled || !otherCanceled)
                    cancelPromise.resolve(undefined);
            },
            errorSteps: () => {
                reading = false;
            },
        });
    };
    const pull1Algorithm = () => {
        if (reading) {
            readAgainForBranch1 = true;
            return resolvedPromise(undefined);
        }
        reading = true;
        const byobRequest = readableByteStreamControllerGetBYOBRequest(branch1.controller);
        if (byobRequest === null)
            pullWithDefaultReader();
        else
            pullWithBYOBReader(byobRequest.view, false);
        return resolvedPromise(undefined);
    };
    const pull2Algorithm = () => {
        if (reading) {
            readAgainForBranch2 = true;
            return resolvedPromise(undefined);
        }
        reading = true;
        const byobRequest = readableByteStreamControllerGetBYOBRequest(branch2.controller);
        if (byobRequest === null)
            pullWithDefaultReader();
        else
            pullWithBYOBReader(byobRequest.view, true);
        return resolvedPromise(undefined);
    };
    const cancel1Algorithm = (reason) => {
        canceled1 = true;
        reason1 = reason;
        if (canceled2)
            cancelPromise.resolve(readableStreamCancel(stream, [reason1, reason2]));
        return cancelPromise.promise;
    };
    const cancel2Algorithm = (reason) => {
        canceled2 = true;
        reason2 = reason;
        if (canceled1)
            cancelPromise.resolve(readableStreamCancel(stream, [reason1, reason2]));
        return cancelPromise.promise;
    };
    const startAlgorithm = () => undefined;
    branch1 = createReadableByteStream(startAlgorithm, pull1Algorithm, cancel1Algorithm);
    branch2 = createReadableByteStream(startAlgorithm, pull2Algorithm, cancel2Algorithm);
    forwardReaderError(reader);
    return [branch1, branch2];
}

// --- Async iteration ------------------------------------------------------------------------

const END_OF_ITERATION = {};

class ReadableStreamAsyncIterator {
    #s;
    static { iteratorSlots = (o) => (isObject(o) && #s in o) ? o.#s : undefined; }

    constructor(token, reader, preventCancel)
    {
        if (token !== INTERNAL)
            throw new TypeError_("Illegal constructor");
        this.#s = { reader, preventCancel, ongoingPromise: undefined, isFinished: false };
    }

    next()
    {
        const iterator = iteratorSlots(this);
        if (!iterator)
            return rejectedPromise(new TypeError_("next() called on an object that is not a ReadableStream async iterator"));
        const nextSteps = () => {
            if (iterator.isFinished)
                return resolvedPromise({ value: undefined, done: true });
            const nextPromise = readableStreamAsyncIteratorGetNext(iterator);
            return react(nextPromise, (value) => {
                iterator.ongoingPromise = undefined;
                if (value === END_OF_ITERATION) {
                    iterator.isFinished = true;
                    return { value: undefined, done: true };
                }
                return { value, done: false };
            }, (reason) => {
                iterator.ongoingPromise = undefined;
                iterator.isFinished = true;
                throw reason;
            });
        };
        iterator.ongoingPromise = iterator.ongoingPromise ? react(iterator.ongoingPromise, nextSteps, nextSteps) : nextSteps();
        return iterator.ongoingPromise;
    }

    return(value = undefined)
    {
        const iterator = iteratorSlots(this);
        if (!iterator)
            return rejectedPromise(new TypeError_("return() called on an object that is not a ReadableStream async iterator"));
        const returnSteps = () => {
            if (iterator.isFinished)
                return resolvedPromise({ value, done: true });
            iterator.isFinished = true;
            return react(readableStreamAsyncIteratorReturn(iterator, value), () => ({ value, done: true }), undefined);
        };
        const promise = iterator.ongoingPromise ? react(iterator.ongoingPromise, returnSteps, returnSteps) : returnSteps();
        return promise;
    }
}

function readableStreamAsyncIteratorGetNext(iterator)
{
    const reader = iterator.reader;
    const promise = newPromise();
    if (reader.stream === undefined) {
        promise.reject(new TypeError_("Cannot get the next iteration result once the reader has been released"));
        return promise.promise;
    }
    readableStreamDefaultReaderRead(reader, {
        chunkSteps: (chunk) => promise.resolve(chunk),
        closeSteps: () => {
            readableStreamDefaultReaderRelease(reader);
            promise.resolve(END_OF_ITERATION);
        },
        errorSteps: (error) => {
            readableStreamDefaultReaderRelease(reader);
            promise.reject(error);
        },
    });
    return promise.promise;
}

function readableStreamAsyncIteratorReturn(iterator, value)
{
    const reader = iterator.reader;
    if (reader.stream === undefined)
        return resolvedPromise(undefined);
    if (!iterator.preventCancel) {
        const result = readableStreamCancel(reader.stream, value);
        readableStreamDefaultReaderRelease(reader);
        return result;
    }
    readableStreamDefaultReaderRelease(reader);
    return resolvedPromise(undefined);
}

// --- WritableStream -------------------------------------------------------------------------------

let writableSlots;
let writerSlots;
let writableControllerSlots;
const closeSentinel = {};

function writableStreamRecord(object)
{
    return {
        object,
        state: "writable",
        storedError: undefined,
        writer: undefined,
        controller: undefined,
        inFlightWriteRequest: undefined,
        closeRequest: undefined,
        inFlightCloseRequest: undefined,
        pendingAbortRequest: undefined,
        writeRequests: new Queue(),
        backpressure: false,
        detached: false,
    };
}

class WritableStream {
    #s;
    static { writableSlots = (o) => (isObject(o) && #s in o) ? o.#s : undefined; }

    constructor(underlyingSink = undefined, strategy = undefined)
    {
        if (underlyingSink === INTERNAL) {
            this.#s = writableStreamRecord(this);
            return;
        }
        const sink = underlyingSink === undefined ? null : underlyingSink;
        const dict = dictionary(sink, "UnderlyingSink");
        const abort = callbackMember(dict, "abort", "UnderlyingSink");
        const close = callbackMember(dict, "close", "UnderlyingSink");
        const start = callbackMember(dict, "start", "UnderlyingSink");
        const type = dict.type;
        const write = callbackMember(dict, "write", "UnderlyingSink");
        if (type !== undefined)
            throw new RangeError_("Invalid type is specified");
        const strategyDict = readStrategy(strategy);
        this.#s = writableStreamRecord(this);
        const sizeAlgorithm = extractSizeAlgorithm(strategyDict);
        const highWaterMark = extractHighWaterMark(strategyDict, 1);
        setUpWritableStreamDefaultControllerFromUnderlyingSink(this.#s, sink, { abort, close, start, write }, highWaterMark, sizeAlgorithm);
    }

    get locked()
    {
        const stream = writableSlots(this);
        if (!stream)
            throw new TypeError_("Illegal invocation");
        return isWritableStreamLocked(stream);
    }

    abort(reason = undefined)
    {
        const stream = writableSlots(this);
        if (!stream)
            return rejectedPromise(new TypeError_("Illegal invocation"));
        if (isWritableStreamLocked(stream))
            return rejectedPromise(new TypeError_("Cannot abort a stream that already has a writer"));
        return writableStreamAbort(stream, reason);
    }

    close()
    {
        const stream = writableSlots(this);
        if (!stream)
            return rejectedPromise(new TypeError_("Illegal invocation"));
        if (isWritableStreamLocked(stream))
            return rejectedPromise(new TypeError_("Cannot close a stream that already has a writer"));
        if (writableStreamCloseQueuedOrInFlight(stream))
            return rejectedPromise(new TypeError_("Cannot close an already-closing stream"));
        return writableStreamClose(stream);
    }

    getWriter()
    {
        const stream = writableSlots(this);
        if (!stream)
            throw new TypeError_("Illegal invocation");
        return acquireWritableStreamDefaultWriter(stream).object;
    }
}

function createWritableStream(startAlgorithm, writeAlgorithm, closeAlgorithm, abortAlgorithm, highWaterMark, sizeAlgorithm)
{
    const stream = writableSlots(new WritableStream(INTERNAL));
    const controller = writableControllerSlots(new WritableStreamDefaultController(INTERNAL));
    setUpWritableStreamDefaultController(stream, controller, startAlgorithm, writeAlgorithm, closeAlgorithm, abortAlgorithm, highWaterMark, sizeAlgorithm);
    return stream;
}

function isWritableStreamLocked(stream)
{
    return stream.writer !== undefined;
}

function acquireWritableStreamDefaultWriter(stream)
{
    const writer = writerSlots(new WritableStreamDefaultWriter(INTERNAL));
    setUpWritableStreamDefaultWriter(writer, stream);
    return writer;
}

function writableStreamAbort(stream, reason)
{
    if (stream.state === "closed" || stream.state === "errored")
        return resolvedPromise(undefined);
    const abortController = stream.controller.abortController;
    if (abortController)
        ReflectApply(abortControllerAbort, abortController, [reason]);
    const state = stream.state;
    if (state === "closed" || state === "errored")
        return resolvedPromise(undefined);
    if (stream.pendingAbortRequest !== undefined)
        return stream.pendingAbortRequest.promise.promise;
    let wasAlreadyErroring = false;
    if (state === "erroring") {
        wasAlreadyErroring = true;
        reason = undefined;
    }
    const promise = newPromise();
    stream.pendingAbortRequest = { promise, reason, wasAlreadyErroring };
    if (!wasAlreadyErroring)
        writableStreamStartErroring(stream, reason);
    return promise.promise;
}

function writableStreamClose(stream)
{
    const state = stream.state;
    if (state === "closed" || state === "errored")
        return rejectedPromise(new TypeError_(`The stream (in ${state} state) is not in the writable state and cannot be closed`));
    const promise = newPromise();
    stream.closeRequest = promise;
    const writer = stream.writer;
    if (writer !== undefined && stream.backpressure && state === "writable")
        writer.readyPromise.resolve(undefined);
    writableStreamDefaultControllerClose(stream.controller);
    return promise.promise;
}

function writableStreamAddWriteRequest(stream)
{
    const promise = newPromise();
    stream.writeRequests.push(promise);
    return promise.promise;
}

function writableStreamCloseQueuedOrInFlight(stream)
{
    return stream.closeRequest !== undefined || stream.inFlightCloseRequest !== undefined;
}

function writableStreamDealWithRejection(stream, error)
{
    if (stream.state === "writable") {
        writableStreamStartErroring(stream, error);
        return;
    }
    writableStreamFinishErroring(stream);
}

function writableStreamFinishErroring(stream)
{
    stream.state = "errored";
    stream.controller.errorSteps();
    const storedError = stream.storedError;
    for (const request of stream.writeRequests.values())
        request.reject(storedError);
    stream.writeRequests = new Queue();
    if (stream.pendingAbortRequest === undefined) {
        writableStreamRejectCloseAndClosedPromiseIfNeeded(stream);
        return;
    }
    const abortRequest = stream.pendingAbortRequest;
    stream.pendingAbortRequest = undefined;
    if (abortRequest.wasAlreadyErroring) {
        abortRequest.promise.reject(storedError);
        writableStreamRejectCloseAndClosedPromiseIfNeeded(stream);
        return;
    }
    const promise = stream.controller.abortSteps(abortRequest.reason);
    react(promise, () => {
        abortRequest.promise.resolve(undefined);
        writableStreamRejectCloseAndClosedPromiseIfNeeded(stream);
    }, (reason) => {
        abortRequest.promise.reject(reason);
        writableStreamRejectCloseAndClosedPromiseIfNeeded(stream);
    });
}

function writableStreamFinishInFlightClose(stream)
{
    stream.inFlightCloseRequest.resolve(undefined);
    stream.inFlightCloseRequest = undefined;
    if (stream.state === "erroring") {
        stream.storedError = undefined;
        if (stream.pendingAbortRequest !== undefined) {
            stream.pendingAbortRequest.promise.resolve(undefined);
            stream.pendingAbortRequest = undefined;
        }
    }
    stream.state = "closed";
    const writer = stream.writer;
    if (writer !== undefined)
        writer.closedPromise.resolve(undefined);
}

function writableStreamFinishInFlightCloseWithError(stream, error)
{
    stream.inFlightCloseRequest.reject(error);
    stream.inFlightCloseRequest = undefined;
    if (stream.pendingAbortRequest !== undefined) {
        stream.pendingAbortRequest.promise.reject(error);
        stream.pendingAbortRequest = undefined;
    }
    writableStreamDealWithRejection(stream, error);
}

function writableStreamFinishInFlightWrite(stream)
{
    stream.inFlightWriteRequest.resolve(undefined);
    stream.inFlightWriteRequest = undefined;
}

function writableStreamFinishInFlightWriteWithError(stream, error)
{
    stream.inFlightWriteRequest.reject(error);
    stream.inFlightWriteRequest = undefined;
    writableStreamDealWithRejection(stream, error);
}

function writableStreamHasOperationMarkedInFlight(stream)
{
    return stream.inFlightWriteRequest !== undefined || stream.inFlightCloseRequest !== undefined;
}

function writableStreamMarkCloseRequestInFlight(stream)
{
    stream.inFlightCloseRequest = stream.closeRequest;
    stream.closeRequest = undefined;
}

function writableStreamMarkFirstWriteRequestInFlight(stream)
{
    stream.inFlightWriteRequest = stream.writeRequests.shift();
}

function writableStreamRejectCloseAndClosedPromiseIfNeeded(stream)
{
    if (stream.closeRequest !== undefined) {
        stream.closeRequest.reject(stream.storedError);
        stream.closeRequest = undefined;
    }
    const writer = stream.writer;
    if (writer !== undefined) {
        writer.closedPromise.reject(stream.storedError);
        setHandled(writer.closedPromise.promise);
    }
}

function writableStreamStartErroring(stream, reason)
{
    const controller = stream.controller;
    stream.state = "erroring";
    stream.storedError = reason;
    const writer = stream.writer;
    if (writer !== undefined)
        writableStreamDefaultWriterEnsureReadyPromiseRejected(writer, reason);
    if (!writableStreamHasOperationMarkedInFlight(stream) && controller.started)
        writableStreamFinishErroring(stream);
}

function writableStreamUpdateBackpressure(stream, backpressure)
{
    const writer = stream.writer;
    if (writer !== undefined && backpressure !== stream.backpressure) {
        if (backpressure)
            writer.readyPromise = newPromise();
        else
            writer.readyPromise.resolve(undefined);
    }
    stream.backpressure = backpressure;
}

// --- The writer -------------------------------------------------------------------------------

class WritableStreamDefaultWriter {
    #s;
    static { writerSlots = (o) => (isObject(o) && #s in o) ? o.#s : undefined; }

    constructor(stream)
    {
        this.#s = { object: this, stream: undefined, closedPromise: undefined, readyPromise: undefined };
        if (stream === INTERNAL)
            return;
        const record = writableSlots(stream);
        if (!record)
            throw new TypeError_("Failed to construct 'WritableStreamDefaultWriter': parameter 1 is not of type 'WritableStream'.");
        setUpWritableStreamDefaultWriter(this.#s, record);
    }

    get closed()
    {
        const writer = writerSlots(this);
        if (!writer)
            return rejectedPromise(new TypeError_("Illegal invocation"));
        return writer.closedPromise.promise;
    }

    get desiredSize()
    {
        const writer = writerSlots(this);
        if (!writer)
            throw new TypeError_("Illegal invocation");
        if (writer.stream === undefined)
            throw new TypeError_("This writer has been released and cannot be used to get the desired size");
        return writableStreamDefaultWriterGetDesiredSize(writer);
    }

    get ready()
    {
        const writer = writerSlots(this);
        if (!writer)
            return rejectedPromise(new TypeError_("Illegal invocation"));
        return writer.readyPromise.promise;
    }

    abort(reason = undefined)
    {
        const writer = writerSlots(this);
        if (!writer)
            return rejectedPromise(new TypeError_("Illegal invocation"));
        if (writer.stream === undefined)
            return rejectedPromise(new TypeError_("This writer has been released and cannot be used to abort"));
        return writableStreamAbort(writer.stream, reason);
    }

    close()
    {
        const writer = writerSlots(this);
        if (!writer)
            return rejectedPromise(new TypeError_("Illegal invocation"));
        const stream = writer.stream;
        if (stream === undefined)
            return rejectedPromise(new TypeError_("This writer has been released and cannot be used to close"));
        if (writableStreamCloseQueuedOrInFlight(stream))
            return rejectedPromise(new TypeError_("Cannot close an already-closing stream"));
        return writableStreamClose(stream);
    }

    releaseLock()
    {
        const writer = writerSlots(this);
        if (!writer)
            throw new TypeError_("Illegal invocation");
        if (writer.stream === undefined)
            return;
        writableStreamDefaultWriterRelease(writer);
    }

    write(chunk = undefined)
    {
        const writer = writerSlots(this);
        if (!writer)
            return rejectedPromise(new TypeError_("Illegal invocation"));
        if (writer.stream === undefined)
            return rejectedPromise(new TypeError_("This writer has been released and cannot be used to write"));
        return writableStreamDefaultWriterWrite(writer, chunk);
    }
}

function setUpWritableStreamDefaultWriter(writer, stream)
{
    if (isWritableStreamLocked(stream))
        throw new TypeError_("This stream has already been locked for exclusive writing by another writer");
    writer.stream = stream;
    stream.writer = writer;
    const state = stream.state;
    writer.readyPromise = newPromise();
    writer.closedPromise = newPromise();
    if (state === "writable") {
        if (writableStreamCloseQueuedOrInFlight(stream) || !stream.backpressure)
            writer.readyPromise.resolve(undefined);
    } else if (state === "erroring") {
        writer.readyPromise.reject(stream.storedError);
        setHandled(writer.readyPromise.promise);
    } else if (state === "closed") {
        writer.readyPromise.resolve(undefined);
        writer.closedPromise.resolve(undefined);
    } else {
        writer.readyPromise.reject(stream.storedError);
        setHandled(writer.readyPromise.promise);
        writer.closedPromise.reject(stream.storedError);
        setHandled(writer.closedPromise.promise);
    }
}

function writableStreamDefaultWriterCloseWithErrorPropagation(writer)
{
    const stream = writer.stream;
    const state = stream.state;
    if (writableStreamCloseQueuedOrInFlight(stream) || state === "closed")
        return resolvedPromise(undefined);
    if (state === "errored")
        return rejectedPromise(stream.storedError);
    return writableStreamClose(stream);
}

function writableStreamDefaultWriterEnsureClosedPromiseRejected(writer, error)
{
    if (writer.closedPromise.state === "pending") {
        writer.closedPromise.reject(error);
    } else {
        writer.closedPromise = newPromise();
        writer.closedPromise.reject(error);
    }
    setHandled(writer.closedPromise.promise);
}

function writableStreamDefaultWriterEnsureReadyPromiseRejected(writer, error)
{
    if (writer.readyPromise.state === "pending") {
        writer.readyPromise.reject(error);
    } else {
        writer.readyPromise = newPromise();
        writer.readyPromise.reject(error);
    }
    setHandled(writer.readyPromise.promise);
}

function writableStreamDefaultWriterGetDesiredSize(writer)
{
    const stream = writer.stream;
    const state = stream.state;
    if (state === "errored" || state === "erroring")
        return null;
    if (state === "closed")
        return 0;
    return writableStreamDefaultControllerGetDesiredSize(stream.controller);
}

function writableStreamDefaultWriterRelease(writer)
{
    const stream = writer.stream;
    const releasedError = new TypeError_("Writer was released and can no longer be used to monitor the stream's closedness");
    writableStreamDefaultWriterEnsureReadyPromiseRejected(writer, releasedError);
    writableStreamDefaultWriterEnsureClosedPromiseRejected(writer, releasedError);
    stream.writer = undefined;
    writer.stream = undefined;
}

function writableStreamDefaultWriterWrite(writer, chunk)
{
    const stream = writer.stream;
    const controller = stream.controller;
    const chunkSize = writableStreamDefaultControllerGetChunkSize(controller, chunk);
    if (stream !== writer.stream)
        return rejectedPromise(new TypeError_("The writer was released while the chunk's size was measured"));
    const state = stream.state;
    if (state === "errored")
        return rejectedPromise(stream.storedError);
    if (writableStreamCloseQueuedOrInFlight(stream) || state === "closed")
        return rejectedPromise(new TypeError_("The stream is closing or closed and cannot be written to"));
    if (state === "erroring")
        return rejectedPromise(stream.storedError);
    const promise = writableStreamAddWriteRequest(stream);
    writableStreamDefaultControllerWrite(controller, chunk, chunkSize);
    return promise;
}

// --- The writable controller ----------------------------------------------------------------------

class WritableStreamDefaultController {
    #s;
    static { writableControllerSlots = (o) => (isObject(o) && #s in o) ? o.#s : undefined; }

    constructor(token = undefined)
    {
        if (token !== INTERNAL)
            throw new TypeError_("Illegal constructor");
        this.#s = {
            object: this,
            stream: undefined,
            queue: new Queue(),
            queueTotalSize: 0,
            abortController: undefined,
            started: false,
            strategyHWM: 1,
            strategySizeAlgorithm: undefined,
            writeAlgorithm: undefined,
            closeAlgorithm: undefined,
            abortAlgorithm: undefined,
            abortSteps: undefined,
            errorSteps: undefined,
        };
        const controller = this.#s;
        controller.abortSteps = (reason) => {
            const result = controller.abortAlgorithm(reason);
            writableStreamDefaultControllerClearAlgorithms(controller);
            return result;
        };
        controller.errorSteps = () => resetQueue(controller);
    }

    get signal()
    {
        const controller = writableControllerSlots(this);
        if (!controller)
            throw new TypeError_("Illegal invocation");
        return controller.abortController ? ReflectApply(abortControllerSignal, controller.abortController, []) : undefined;
    }

    error(e = undefined)
    {
        const controller = writableControllerSlots(this);
        if (!controller)
            throw new TypeError_("Illegal invocation");
        if (controller.stream.state !== "writable")
            return;
        writableStreamDefaultControllerError(controller, e);
    }
}

function setUpWritableStreamDefaultController(stream, controller, startAlgorithm, writeAlgorithm, closeAlgorithm, abortAlgorithm, highWaterMark, sizeAlgorithm)
{
    controller.stream = stream;
    stream.controller = controller;
    resetQueue(controller);
    controller.abortController = AbortController_ ? new AbortController_() : undefined;
    controller.started = false;
    controller.strategySizeAlgorithm = sizeAlgorithm;
    controller.strategyHWM = highWaterMark;
    controller.writeAlgorithm = writeAlgorithm;
    controller.closeAlgorithm = closeAlgorithm;
    controller.abortAlgorithm = abortAlgorithm;
    const backpressure = writableStreamDefaultControllerGetBackpressure(controller);
    writableStreamUpdateBackpressure(stream, backpressure);
    const startResult = startAlgorithm();
    react(resolvedPromise(startResult), () => {
        controller.started = true;
        writableStreamDefaultControllerAdvanceQueueIfNeeded(controller);
    }, (reason) => {
        controller.started = true;
        writableStreamDealWithRejection(stream, reason);
    });
}

function setUpWritableStreamDefaultControllerFromUnderlyingSink(stream, sink, dict, highWaterMark, sizeAlgorithm)
{
    const controller = writableControllerSlots(new WritableStreamDefaultController(INTERNAL));
    let startAlgorithm = () => undefined;
    let writeAlgorithm = () => resolvedPromise(undefined);
    let closeAlgorithm = () => resolvedPromise(undefined);
    let abortAlgorithm = () => resolvedPromise(undefined);
    if (dict.start !== undefined)
        startAlgorithm = () => ReflectApply(dict.start, sink, [controller.object]);
    if (dict.write !== undefined)
        writeAlgorithm = (chunk) => promiseCall(dict.write, sink, [chunk, controller.object]);
    if (dict.close !== undefined)
        closeAlgorithm = () => promiseCall(dict.close, sink, []);
    if (dict.abort !== undefined)
        abortAlgorithm = (reason) => promiseCall(dict.abort, sink, [reason]);
    setUpWritableStreamDefaultController(stream, controller, startAlgorithm, writeAlgorithm, closeAlgorithm, abortAlgorithm, highWaterMark, sizeAlgorithm);
}

function writableStreamDefaultControllerAdvanceQueueIfNeeded(controller)
{
    const stream = controller.stream;
    if (!controller.started)
        return;
    if (stream.inFlightWriteRequest !== undefined)
        return;
    const state = stream.state;
    if (state === "closed" || state === "errored")
        return;
    if (state === "erroring") {
        writableStreamFinishErroring(stream);
        return;
    }
    if (controller.queue.length === 0)
        return;
    const value = peekQueueValue(controller);
    if (value === closeSentinel)
        writableStreamDefaultControllerProcessClose(controller);
    else
        writableStreamDefaultControllerProcessWrite(controller, value);
}

function writableStreamDefaultControllerClearAlgorithms(controller)
{
    controller.writeAlgorithm = undefined;
    controller.closeAlgorithm = undefined;
    controller.abortAlgorithm = undefined;
    controller.strategySizeAlgorithm = undefined;
}

function writableStreamDefaultControllerClose(controller)
{
    enqueueValueWithSize(controller, closeSentinel, 0);
    writableStreamDefaultControllerAdvanceQueueIfNeeded(controller);
}

function writableStreamDefaultControllerError(controller, error)
{
    const stream = controller.stream;
    writableStreamDefaultControllerClearAlgorithms(controller);
    writableStreamStartErroring(stream, error);
}

function writableStreamDefaultControllerErrorIfNeeded(controller, error)
{
    if (controller.stream.state === "writable")
        writableStreamDefaultControllerError(controller, error);
}

function writableStreamDefaultControllerGetBackpressure(controller)
{
    return writableStreamDefaultControllerGetDesiredSize(controller) <= 0;
}

function writableStreamDefaultControllerGetChunkSize(controller, chunk)
{
    if (controller.strategySizeAlgorithm === undefined)
        return 1;
    try {
        return controller.strategySizeAlgorithm(chunk);
    } catch (error) {
        writableStreamDefaultControllerErrorIfNeeded(controller, error);
        return 1;
    }
}

function writableStreamDefaultControllerGetDesiredSize(controller)
{
    return controller.strategyHWM - controller.queueTotalSize;
}

function writableStreamDefaultControllerProcessClose(controller)
{
    const stream = controller.stream;
    writableStreamMarkCloseRequestInFlight(stream);
    dequeueValue(controller);
    const sinkClosePromise = controller.closeAlgorithm();
    writableStreamDefaultControllerClearAlgorithms(controller);
    react(sinkClosePromise, () => {
        writableStreamFinishInFlightClose(stream);
    }, (reason) => {
        writableStreamFinishInFlightCloseWithError(stream, reason);
    });
}

function writableStreamDefaultControllerProcessWrite(controller, chunk)
{
    const stream = controller.stream;
    writableStreamMarkFirstWriteRequestInFlight(stream);
    const sinkWritePromise = controller.writeAlgorithm(chunk);
    react(sinkWritePromise, () => {
        writableStreamFinishInFlightWrite(stream);
        const state = stream.state;
        dequeueValue(controller);
        if (!writableStreamCloseQueuedOrInFlight(stream) && state === "writable") {
            const backpressure = writableStreamDefaultControllerGetBackpressure(controller);
            writableStreamUpdateBackpressure(stream, backpressure);
        }
        writableStreamDefaultControllerAdvanceQueueIfNeeded(controller);
    }, (reason) => {
        if (stream.state === "writable")
            writableStreamDefaultControllerClearAlgorithms(controller);
        writableStreamFinishInFlightWriteWithError(stream, reason);
    });
}

function writableStreamDefaultControllerWrite(controller, chunk, chunkSize)
{
    try {
        enqueueValueWithSize(controller, chunk, chunkSize);
    } catch (error) {
        writableStreamDefaultControllerErrorIfNeeded(controller, error);
        return;
    }
    const stream = controller.stream;
    if (!writableStreamCloseQueuedOrInFlight(stream) && stream.state === "writable") {
        const backpressure = writableStreamDefaultControllerGetBackpressure(controller);
        writableStreamUpdateBackpressure(stream, backpressure);
    }
    writableStreamDefaultControllerAdvanceQueueIfNeeded(controller);
}

// --- Piping ---------------------------------------------------------------------------------------

function readableStreamPipeTo(source, dest, preventClose, preventAbort, preventCancel, signal)
{
    const reader = acquireReadableStreamDefaultReader(source);
    const writer = acquireWritableStreamDefaultWriter(dest);
    source.disturbed = true;
    let shuttingDown = false;
    let currentWrite = resolvedPromise(undefined);
    const promise = newPromise();
    let abortAlgorithm;

    const finalize = (isError, error) => {
        writableStreamDefaultWriterRelease(writer);
        readableStreamDefaultReaderRelease(reader);
        if (signal !== undefined && abortAlgorithm !== undefined)
            ReflectApply(eventTargetRemove, signal, ["abort", abortAlgorithm]);
        if (isError)
            promise.reject(error);
        else
            promise.resolve(undefined);
    };
    const waitForWritesToFinish = () => {
        const oldCurrentWrite = currentWrite;
        return react(oldCurrentWrite, () => oldCurrentWrite !== currentWrite ? waitForWritesToFinish() : undefined, undefined);
    };
    const shutdownWithAction = (action, originalIsError, originalError) => {
        if (shuttingDown)
            return;
        shuttingDown = true;
        const doTheRest = () => {
            react(action(), () => finalize(originalIsError, originalError), (newError) => finalize(true, newError));
        };
        if (dest.state === "writable" && !writableStreamCloseQueuedOrInFlight(dest))
            uponFulfillment(waitForWritesToFinish(), doTheRest);
        else
            doTheRest();
    };
    const shutdown = (isError, error) => {
        if (shuttingDown)
            return;
        shuttingDown = true;
        if (dest.state === "writable" && !writableStreamCloseQueuedOrInFlight(dest))
            uponFulfillment(waitForWritesToFinish(), () => finalize(isError, error));
        else
            finalize(isError, error);
    };

    if (signal !== undefined) {
        abortAlgorithm = () => {
            const error = ReflectApply(abortSignalReason, signal, []);
            const actions = [];
            if (!preventAbort) {
                actions.push(() => {
                    if (dest.state === "writable")
                        return writableStreamAbort(dest, error);
                    return resolvedPromise(undefined);
                });
            }
            if (!preventCancel) {
                actions.push(() => {
                    if (source.state === "readable")
                        return readableStreamCancel(source, error);
                    return resolvedPromise(undefined);
                });
            }
            shutdownWithAction(() => {
                const promises = [];
                for (const action of actions)
                    promises.push(action());
                return Promise_.all(promises);
            }, true, error);
        };
        if (ReflectApply(abortSignalAborted, signal, [])) {
            abortAlgorithm();
            return promise.promise;
        }
        ReflectApply(eventTargetAdd, signal, ["abort", abortAlgorithm]);
    }

    // Errors and closes, in either direction, as they are or as they come.
    const isOrBecomesErrored = (stream, closedPromise, action) => {
        if (stream.state === "errored")
            action(stream.storedError);
        else
            uponRejection(closedPromise, action);
    };
    const isOrBecomesClosed = (stream, closedPromise, action) => {
        if (stream.state === "closed")
            action();
        else
            uponFulfillment(closedPromise, action);
    };
    isOrBecomesErrored(source, reader.closedPromise.promise, (storedError) => {
        if (!preventAbort)
            shutdownWithAction(() => writableStreamAbort(dest, storedError), true, storedError);
        else
            shutdown(true, storedError);
    });
    isOrBecomesErrored(dest, writer.closedPromise.promise, (storedError) => {
        if (!preventCancel)
            shutdownWithAction(() => readableStreamCancel(source, storedError), true, storedError);
        else
            shutdown(true, storedError);
    });
    isOrBecomesClosed(source, reader.closedPromise.promise, () => {
        if (!preventClose)
            shutdownWithAction(() => writableStreamDefaultWriterCloseWithErrorPropagation(writer));
        else
            shutdown();
    });
    if (writableStreamCloseQueuedOrInFlight(dest) || dest.state === "closed") {
        const destClosed = new TypeError_("the destination writable stream closed before all data could be piped to it");
        if (!preventCancel)
            shutdownWithAction(() => readableStreamCancel(source, destClosed), true, destClosed);
        else
            shutdown(true, destClosed);
    }
    setHandled(promise.promise);

    // The loop: wait for the destination to want more, read, write.
    const pipeStep = () => {
        if (shuttingDown)
            return;
        uponFulfillment(writer.readyPromise.promise, () => {
            if (shuttingDown)
                return;
            readableStreamDefaultReaderRead(reader, {
                chunkSteps: (chunk) => {
                    currentWrite = react(writableStreamDefaultWriterWrite(writer, chunk), undefined, () => {});
                    pipeStep();
                },
                closeSteps: () => {},
                errorSteps: () => {},
            });
        });
    };
    pipeStep();
    return promise.promise;
}

// --- TransformStream --------------------------------------------------------------------------------

let transformSlots;
let transformControllerSlots;

class TransformStream {
    #s;
    static { transformSlots = (o) => (isObject(o) && #s in o) ? o.#s : undefined; }

    constructor(transformer = undefined, writableStrategy = undefined, readableStrategy = undefined)
    {
        const transformerObject = transformer === undefined ? null : transformer;
        const dict = dictionary(transformerObject, "Transformer");
        const cancel = callbackMember(dict, "cancel", "Transformer");
        const flush = callbackMember(dict, "flush", "Transformer");
        const readableType = dict.readableType;
        const start = callbackMember(dict, "start", "Transformer");
        const transform = callbackMember(dict, "transform", "Transformer");
        const writableType = dict.writableType;
        const writableStrategyDict = readStrategy(writableStrategy);
        const readableStrategyDict = readStrategy(readableStrategy);
        if (readableType !== undefined)
            throw new RangeError_("Invalid readableType specified");
        if (writableType !== undefined)
            throw new RangeError_("Invalid writableType specified");
        const readableHighWaterMark = extractHighWaterMark(readableStrategyDict, 0);
        const readableSizeAlgorithm = extractSizeAlgorithm(readableStrategyDict);
        const writableHighWaterMark = extractHighWaterMark(writableStrategyDict, 1);
        const writableSizeAlgorithm = extractSizeAlgorithm(writableStrategyDict);
        const startPromise = newPromise();
        this.#s = { object: this, backpressure: undefined, backpressureChangePromise: undefined, controller: undefined, readable: undefined, writable: undefined, detached: false };
        initializeTransformStream(this.#s, startPromise, writableHighWaterMark, writableSizeAlgorithm, readableHighWaterMark, readableSizeAlgorithm);
        setUpTransformStreamDefaultControllerFromTransformer(this.#s, transformerObject, { cancel, flush, start, transform });
        if (start !== undefined)
            startPromise.resolve(ReflectApply(start, transformerObject, [this.#s.controller.object]));
        else
            startPromise.resolve(undefined);
    }

    get readable()
    {
        const stream = transformSlots(this);
        if (!stream)
            throw new TypeError_("Illegal invocation");
        return stream.readable.object;
    }

    get writable()
    {
        const stream = transformSlots(this);
        if (!stream)
            throw new TypeError_("Illegal invocation");
        return stream.writable.object;
    }
}

class TransformStreamDefaultController {
    #s;
    static { transformControllerSlots = (o) => (isObject(o) && #s in o) ? o.#s : undefined; }

    constructor(token = undefined)
    {
        if (token !== INTERNAL)
            throw new TypeError_("Illegal constructor");
        this.#s = { object: this, stream: undefined, transformAlgorithm: undefined, flushAlgorithm: undefined, cancelAlgorithm: undefined, finishPromise: undefined };
    }

    get desiredSize()
    {
        const controller = transformControllerSlots(this);
        if (!controller)
            throw new TypeError_("Illegal invocation");
        return readableStreamDefaultControllerGetDesiredSize(controller.stream.readable.controller);
    }

    enqueue(chunk = undefined)
    {
        const controller = transformControllerSlots(this);
        if (!controller)
            throw new TypeError_("Illegal invocation");
        transformStreamDefaultControllerEnqueue(controller, chunk);
    }

    error(reason = undefined)
    {
        const controller = transformControllerSlots(this);
        if (!controller)
            throw new TypeError_("Illegal invocation");
        transformStreamDefaultControllerError(controller, reason);
    }

    terminate()
    {
        const controller = transformControllerSlots(this);
        if (!controller)
            throw new TypeError_("Illegal invocation");
        transformStreamDefaultControllerTerminate(controller);
    }
}

function initializeTransformStream(stream, startPromise, writableHighWaterMark, writableSizeAlgorithm, readableHighWaterMark, readableSizeAlgorithm)
{
    const startAlgorithm = () => startPromise.promise;
    const writeAlgorithm = (chunk) => transformStreamDefaultSinkWriteAlgorithm(stream, chunk);
    const abortAlgorithm = (reason) => transformStreamDefaultSinkAbortAlgorithm(stream, reason);
    const closeAlgorithm = () => transformStreamDefaultSinkCloseAlgorithm(stream);
    stream.writable = createWritableStream(startAlgorithm, writeAlgorithm, closeAlgorithm, abortAlgorithm, writableHighWaterMark, writableSizeAlgorithm);
    const pullAlgorithm = () => transformStreamDefaultSourcePullAlgorithm(stream);
    const cancelAlgorithm = (reason) => transformStreamDefaultSourceCancelAlgorithm(stream, reason);
    stream.readable = createReadableStream(startAlgorithm, pullAlgorithm, cancelAlgorithm, readableHighWaterMark, readableSizeAlgorithm);
    stream.backpressure = undefined;
    stream.backpressureChangePromise = undefined;
    transformStreamSetBackpressure(stream, true);
    stream.controller = undefined;
}

function transformStreamError(stream, error)
{
    readableStreamDefaultControllerError(stream.readable.controller, error);
    transformStreamErrorWritableAndUnblockWrite(stream, error);
}

function transformStreamErrorWritableAndUnblockWrite(stream, error)
{
    transformStreamDefaultControllerClearAlgorithms(stream.controller);
    writableStreamDefaultControllerErrorIfNeeded(stream.writable.controller, error);
    transformStreamUnblockWrite(stream);
}

function transformStreamUnblockWrite(stream)
{
    if (stream.backpressure)
        transformStreamSetBackpressure(stream, false);
}

function transformStreamSetBackpressure(stream, backpressure)
{
    if (stream.backpressureChangePromise !== undefined)
        stream.backpressureChangePromise.resolve(undefined);
    stream.backpressureChangePromise = newPromise();
    stream.backpressure = backpressure;
}

function setUpTransformStreamDefaultController(stream, controller, transformAlgorithm, flushAlgorithm, cancelAlgorithm)
{
    controller.stream = stream;
    stream.controller = controller;
    controller.transformAlgorithm = transformAlgorithm;
    controller.flushAlgorithm = flushAlgorithm;
    controller.cancelAlgorithm = cancelAlgorithm;
}

function setUpTransformStreamDefaultControllerFromTransformer(stream, transformer, dict)
{
    const controller = transformControllerSlots(new TransformStreamDefaultController(INTERNAL));
    let transformAlgorithm = (chunk) => {
        try {
            transformStreamDefaultControllerEnqueue(controller, chunk);
            return resolvedPromise(undefined);
        } catch (error) {
            return rejectedPromise(error);
        }
    };
    let flushAlgorithm = () => resolvedPromise(undefined);
    let cancelAlgorithm = () => resolvedPromise(undefined);
    if (dict.transform !== undefined)
        transformAlgorithm = (chunk) => promiseCall(dict.transform, transformer, [chunk, controller.object]);
    if (dict.flush !== undefined)
        flushAlgorithm = () => promiseCall(dict.flush, transformer, [controller.object]);
    if (dict.cancel !== undefined)
        cancelAlgorithm = (reason) => promiseCall(dict.cancel, transformer, [reason]);
    setUpTransformStreamDefaultController(stream, controller, transformAlgorithm, flushAlgorithm, cancelAlgorithm);
}

function transformStreamDefaultControllerClearAlgorithms(controller)
{
    controller.transformAlgorithm = undefined;
    controller.flushAlgorithm = undefined;
    controller.cancelAlgorithm = undefined;
}

function transformStreamDefaultControllerEnqueue(controller, chunk)
{
    const stream = controller.stream;
    const readableController = stream.readable.controller;
    if (!readableStreamDefaultControllerCanCloseOrEnqueue(readableController))
        throw new TypeError_("Readable side is not in a state that permits enqueue");
    try {
        readableStreamDefaultControllerEnqueue(readableController, chunk);
    } catch (error) {
        transformStreamErrorWritableAndUnblockWrite(stream, error);
        throw stream.readable.storedError;
    }
    const backpressure = readableStreamDefaultControllerHasBackpressure(readableController);
    if (backpressure !== stream.backpressure)
        transformStreamSetBackpressure(stream, true);
}

function transformStreamDefaultControllerError(controller, error)
{
    transformStreamError(controller.stream, error);
}

function transformStreamDefaultControllerPerformTransform(controller, chunk)
{
    const transformPromise = controller.transformAlgorithm(chunk);
    return react(transformPromise, undefined, (reason) => {
        transformStreamError(controller.stream, reason);
        throw reason;
    });
}

function transformStreamDefaultControllerTerminate(controller)
{
    const stream = controller.stream;
    readableStreamDefaultControllerClose(stream.readable.controller);
    const error = new TypeError_("TransformStream terminated");
    transformStreamErrorWritableAndUnblockWrite(stream, error);
}

function transformStreamDefaultSinkWriteAlgorithm(stream, chunk)
{
    const controller = stream.controller;
    if (stream.backpressure) {
        return react(stream.backpressureChangePromise.promise, () => {
            const writable = stream.writable;
            if (writable.state === "erroring")
                throw writable.storedError;
            return transformStreamDefaultControllerPerformTransform(controller, chunk);
        }, undefined);
    }
    return transformStreamDefaultControllerPerformTransform(controller, chunk);
}

function transformStreamDefaultSinkAbortAlgorithm(stream, reason)
{
    const controller = stream.controller;
    if (controller.finishPromise !== undefined)
        return controller.finishPromise.promise;
    const readable = stream.readable;
    controller.finishPromise = newPromise();
    const cancelPromise = controller.cancelAlgorithm(reason);
    transformStreamDefaultControllerClearAlgorithms(controller);
    react(cancelPromise, () => {
        if (readable.state === "errored") {
            controller.finishPromise.reject(readable.storedError);
        } else {
            readableStreamDefaultControllerError(readable.controller, reason);
            controller.finishPromise.resolve(undefined);
        }
    }, (error) => {
        readableStreamDefaultControllerError(readable.controller, error);
        controller.finishPromise.reject(error);
    });
    return controller.finishPromise.promise;
}

function transformStreamDefaultSinkCloseAlgorithm(stream)
{
    const controller = stream.controller;
    if (controller.finishPromise !== undefined)
        return controller.finishPromise.promise;
    const readable = stream.readable;
    controller.finishPromise = newPromise();
    const flushPromise = controller.flushAlgorithm();
    transformStreamDefaultControllerClearAlgorithms(controller);
    react(flushPromise, () => {
        if (readable.state === "errored") {
            controller.finishPromise.reject(readable.storedError);
        } else {
            readableStreamDefaultControllerClose(readable.controller);
            controller.finishPromise.resolve(undefined);
        }
    }, (error) => {
        readableStreamDefaultControllerError(readable.controller, error);
        controller.finishPromise.reject(error);
    });
    return controller.finishPromise.promise;
}

function transformStreamDefaultSourceCancelAlgorithm(stream, reason)
{
    const controller = stream.controller;
    if (controller.finishPromise !== undefined)
        return controller.finishPromise.promise;
    const writable = stream.writable;
    controller.finishPromise = newPromise();
    const cancelPromise = controller.cancelAlgorithm(reason);
    transformStreamDefaultControllerClearAlgorithms(controller);
    react(cancelPromise, () => {
        if (writable.state === "errored") {
            controller.finishPromise.reject(writable.storedError);
        } else {
            writableStreamDefaultControllerErrorIfNeeded(writable.controller, reason);
            transformStreamUnblockWrite(stream);
            controller.finishPromise.resolve(undefined);
        }
    }, (error) => {
        writableStreamDefaultControllerErrorIfNeeded(writable.controller, error);
        transformStreamUnblockWrite(stream);
        controller.finishPromise.reject(error);
    });
    return controller.finishPromise.promise;
}

function transformStreamDefaultSourcePullAlgorithm(stream)
{
    transformStreamSetBackpressure(stream, false);
    return stream.backpressureChangePromise.promise;
}

// --- The Encoding Standard's streams ------------------------------------------------------------

let encoderStreamSlots;
let decoderStreamSlots;

function newTransform(transformAlgorithm, flushAlgorithm)
{
    const startPromise = newPromise();
    const stream = { object: undefined, backpressure: undefined, backpressureChangePromise: undefined, controller: undefined, readable: undefined, writable: undefined, detached: false };
    initializeTransformStream(stream, startPromise, 1, () => 1, 0, () => 1);
    const controller = transformControllerSlots(new TransformStreamDefaultController(INTERNAL));
    const wrap = (algorithm) => (...args) => {
        try {
            return resolvedPromise(algorithm(controller, ...args));
        } catch (error) {
            return rejectedPromise(error);
        }
    };
    setUpTransformStreamDefaultController(stream, controller, wrap(transformAlgorithm), wrap(flushAlgorithm), () => resolvedPromise(undefined));
    startPromise.resolve(undefined);
    return stream;
}

class TextEncoderStream {
    #s;
    static { encoderStreamSlots = (o) => (isObject(o) && #s in o) ? o.#s : undefined; }

    constructor()
    {
        const encoder = new TextEncoder_();
        const state = { pendingHighSurrogate: null };
        const transform = newTransform((controller, chunk) => {
            let input = `${chunk}`;
            if (state.pendingHighSurrogate !== null) {
                input = state.pendingHighSurrogate + input;
                state.pendingHighSurrogate = null;
            }
            if (input.length > 0) {
                const last = input.charCodeAt(input.length - 1);
                if (last >= 0xD800 && last <= 0xDBFF) {
                    state.pendingHighSurrogate = StringFromCharCode(last);
                    input = input.slice(0, -1);
                }
            }
            if (input.length > 0)
                transformStreamDefaultControllerEnqueue(controller, ReflectApply(textEncoderEncode, encoder, [input]));
        }, (controller) => {
            if (state.pendingHighSurrogate !== null)
                transformStreamDefaultControllerEnqueue(controller, new Uint8Array_([0xEF, 0xBF, 0xBD]));
        });
        this.#s = { transform };
    }

    get encoding()
    {
        if (!encoderStreamSlots(this))
            throw new TypeError_("Illegal invocation");
        return "utf-8";
    }

    get readable()
    {
        const self = encoderStreamSlots(this);
        if (!self)
            throw new TypeError_("Illegal invocation");
        return self.transform.readable.object;
    }

    get writable()
    {
        const self = encoderStreamSlots(this);
        if (!self)
            throw new TypeError_("Illegal invocation");
        return self.transform.writable.object;
    }
}

class TextDecoderStream {
    #s;
    static { decoderStreamSlots = (o) => (isObject(o) && #s in o) ? o.#s : undefined; }

    constructor(label = "utf-8", options = undefined)
    {
        const decoder = new TextDecoder_(label, options);
        const transform = newTransform((controller, chunk) => {
            if (!ArrayBufferIsView(chunk) && !(chunk instanceof ArrayBuffer_))
                throw new TypeError_("Failed to execute 'transform' on 'TextDecoderStream': The provided value is not of type '(ArrayBuffer or ArrayBufferView)'.");
            const output = ReflectApply(textDecoderDecode, decoder, [chunk, { stream: true }]);
            if (output !== "")
                transformStreamDefaultControllerEnqueue(controller, output);
        }, (controller) => {
            const output = ReflectApply(textDecoderDecode, decoder, []);
            if (output !== "")
                transformStreamDefaultControllerEnqueue(controller, output);
        });
        this.#s = { decoder, transform };
    }

    get encoding()
    {
        const self = decoderStreamSlots(this);
        if (!self)
            throw new TypeError_("Illegal invocation");
        return self.decoder.encoding;
    }

    get fatal()
    {
        const self = decoderStreamSlots(this);
        if (!self)
            throw new TypeError_("Illegal invocation");
        return self.decoder.fatal;
    }

    get ignoreBOM()
    {
        const self = decoderStreamSlots(this);
        if (!self)
            throw new TypeError_("Illegal invocation");
        return self.decoder.ignoreBOM;
    }

    get readable()
    {
        const self = decoderStreamSlots(this);
        if (!self)
            throw new TypeError_("Illegal invocation");
        return self.transform.readable.object;
    }

    get writable()
    {
        const self = decoderStreamSlots(this);
        if (!self)
            throw new TypeError_("Illegal invocation");
        return self.transform.writable.object;
    }
}

// --- The interfaces, shaped as WebIDL shapes them ---------------------------------------------

// Operations and attributes are enumerable on a WebIDL prototype, which a
// class's are not; each interface says its name; and each is a global,
// writable and configurable but not enumerable.
function expose(constructor, name)
{
    // (Counted, not iterated: this runs as the script loads, and a page may
    // have replaced the array iterator by then.)
    const prototype = constructor.prototype;
    const keys = ObjectGetOwnPropertyNames(prototype);
    for (let i = 0; i < keys.length; i++) {
        const key = keys[i];
        if (key === "constructor")
            continue;
        const descriptor = ObjectGetOwnPropertyDescriptor(prototype, key);
        descriptor.enumerable = true;
        ObjectDefineProperty(prototype, key, descriptor);
    }
    ObjectDefineProperty(prototype, SymbolToStringTag, { value: name, writable: false, enumerable: false, configurable: true });
    ObjectDefineProperty(global, name, { value: constructor, writable: true, enumerable: false, configurable: true });
}

ObjectDefineProperty(ReadableStream.prototype, SymbolAsyncIterator, {
    value: ReadableStream.prototype.values, writable: true, enumerable: false, configurable: true,
});
expose(ReadableStream, "ReadableStream");
expose(ReadableStreamDefaultReader, "ReadableStreamDefaultReader");
expose(ReadableStreamBYOBReader, "ReadableStreamBYOBReader");
expose(ReadableStreamDefaultController, "ReadableStreamDefaultController");
expose(ReadableByteStreamController, "ReadableByteStreamController");
expose(ReadableStreamBYOBRequest, "ReadableStreamBYOBRequest");
expose(WritableStream, "WritableStream");
expose(WritableStreamDefaultWriter, "WritableStreamDefaultWriter");
expose(WritableStreamDefaultController, "WritableStreamDefaultController");
expose(TransformStream, "TransformStream");
expose(TransformStreamDefaultController, "TransformStreamDefaultController");
expose(ByteLengthQueuingStrategy, "ByteLengthQueuingStrategy");
expose(CountQueuingStrategy, "CountQueuingStrategy");
if (TextEncoder_)
    expose(TextEncoderStream, "TextEncoderStream");
if (TextDecoder_)
    expose(TextDecoderStream, "TextDecoderStream");

// The async iterator's prototype: %AsyncIteratorPrototype% beneath it, its
// own methods enumerable, named as WebIDL names it.
{
    const prototype = ReadableStreamAsyncIterator.prototype;
    ObjectSetPrototypeOf(prototype, AsyncIteratorPrototype);
    const keys = ["next", "return"];
    for (let i = 0; i < keys.length; i++) {
        const key = keys[i];
        const descriptor = ObjectGetOwnPropertyDescriptor(prototype, key);
        descriptor.enumerable = true;
        ObjectDefineProperty(prototype, key, descriptor);
    }
    ObjectDefineProperty(prototype, SymbolToStringTag, { value: "ReadableStream AsyncIterator", writable: false, enumerable: false, configurable: true });
    delete prototype.constructor;
}

// --- What the engine itself asks for -------------------------------------------------------------

return {
    // A byte stream over bytes already in hand: a body the network gave.
    bytesStream(bytes)
    {
        const stream = createReadableByteStream(() => undefined, () => resolvedPromise(undefined), () => resolvedPromise(undefined));
        if (bytes.byteLength > 0)
            readableByteStreamControllerEnqueue(stream.controller, bytes);
        readableByteStreamControllerClose(stream.controller);
        return stream.object;
    },
    isReadableStream(value)
    {
        return streamSlots(value) !== undefined;
    },
    // Whether a stream can still be read whole: not read from, not locked.
    isUnusable(value)
    {
        const stream = streamSlots(value);
        return stream.disturbed || isReadableStreamLocked(stream);
    },
    // Reads a stream to its end: a promise of every byte, in one array.
    readAll(value)
    {
        const stream = streamSlots(value);
        if (stream.disturbed || isReadableStreamLocked(stream))
            return rejectedPromise(new TypeError_("The body has already been read or is locked"));
        const reader = acquireReadableStreamDefaultReader(stream);
        const chunks = [];
        let total = 0;
        const promise = newPromise();
        const step = () => {
            readableStreamDefaultReaderRead(reader, {
                chunkSteps: (chunk) => {
                    if (!(chunk instanceof Uint8Array_)) {
                        promise.reject(new TypeError_("A body stream gave a chunk that is not a Uint8Array"));
                        return;
                    }
                    chunks.push(chunk);
                    total += chunk.byteLength;
                    ReflectApply(queueMicrotask_, global, [step]);
                },
                closeSteps: () => {
                    const all = new Uint8Array_(total);
                    let offset = 0;
                    for (const chunk of chunks) {
                        ReflectApply(typedArraySet, all, [chunk, offset]);
                        offset += chunk.byteLength;
                    }
                    promise.resolve(all);
                },
                errorSteps: (error) => promise.reject(error),
            });
        };
        step();
        return promise.promise;
    },
};
})
