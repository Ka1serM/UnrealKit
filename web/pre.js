//! A folder of another kind mounts with every file in it.
const unrealkitKinds = {
    game: ['.pak', '.utoc', '.ucas', '.sig', '-shipping.exe'],
};

const unrealkitReaderSource = `
onmessage = event => {
    const {files, memory, slot} = event.data;
    const reader = new FileReaderSync();
    postMessage('ready');
    for (;;) {
        let control = new Int32Array(memory.buffer, slot, 6);
        for (let state = Atomics.load(control, 0); state !== 1; state = Atomics.load(control, 0))
            Atomics.wait(control, 0, state);
        control = new Int32Array(memory.buffer, slot, 6);
        const file = files[control[1]];
        const buffer = control[2] >>> 0;
        const size = control[3] >>> 0;
        const offset = new Float64Array(memory.buffer, slot + 24, 1)[0];
        try {
            const data = new Uint8Array(reader.readAsArrayBuffer(file.slice(offset, offset + size)));
            new Uint8Array(memory.buffer).set(data, buffer);
            control[4] = data.length;
        } catch (error) {
            console.error('Reading ' + file.name + ' failed:', error);
            control[4] = -1;
        }
        Atomics.store(control, 0, 2);
        Atomics.notify(control, 0);
    }
};`;

const unrealkitReaderCount = 4;

function unrealkitMountName(name) {
    return name.replaceAll('/', '_');
}

function unrealkitMatches(name, extensions) {
    const lower = name.toLowerCase();
    return !extensions || extensions.some(extension => lower.endsWith(extension));
}

async function unrealkitMount(root, replaced, entries) {
    Module.ccall('unrealkit_mount_remove', null, ['string'], [replaced]);
    const mount = Module.ccall('unrealkit_mount_open', 'number', ['string', 'number'], [root, unrealkitReaderCount]);
    const files = entries.map(entry => entry.file);
    const url = URL.createObjectURL(new Blob([unrealkitReaderSource], {type: 'text/javascript'}));
    try {
        await Promise.all(Array.from({length: unrealkitReaderCount}, (_, reader) => new Promise((resolve, reject) => {
            const worker = new Worker(url);
            worker.onmessage = resolve;
            worker.onerror = reject;
            worker.postMessage({files, memory: wasmMemory, slot: Module._unrealkit_mount_slot(mount, reader)});
        })));
    } finally {
        URL.revokeObjectURL(url);
    }
    entries.forEach((entry, index) => {
        const result = Module.ccall('unrealkit_mount_add', 'number', ['number', 'string', 'number', 'number', 'number'],
                                    [mount, entry.path, index, entry.file.size, entry.file.lastModified]);
        if (result < 0) throw new Error(`Cannot add ${entry.path} to ${root}: error ${-result}.`);
    });
}

async function unrealkitCollect(directory, extensions) {
    const handles = [];
    async function visit(folder, prefix) {
        for await (const entry of folder.values()) {
            if (entry.kind === 'directory') await visit(entry, prefix + entry.name + '/');
            else if (unrealkitMatches(entry.name, extensions)) handles.push({path: prefix + entry.name, handle: entry});
        }
    }
    await visit(directory, '');
    return Promise.all(handles.map(async entry => ({path: entry.path, file: await entry.handle.getFile()})));
}

function unrealkitDatabase() {
    return new Promise((resolve, reject) => {
        const request = indexedDB.open(document.title.toLowerCase(), 3);
        request.onupgradeneeded = () => {
            const database = request.result;
            if (database.objectStoreNames.contains('handles')) database.deleteObjectStore('handles');
            for (const name of ['mounts', 'storage'])
                if (!database.objectStoreNames.contains(name)) database.createObjectStore(name);
        };
        request.onsuccess = () => resolve(request.result);
        request.onerror = () => reject(request.error);
        request.onblocked = () => reject(new Error(`Another tab keeps an older version of the saved data open. Close the other ${document.title} tabs, then reload.`));
    });
}

async function unrealkitStore(name, mode, action) {
    const database = await unrealkitDatabase();
    try {
        return await new Promise((resolve, reject) => {
            const transaction = database.transaction(name, mode);
            const request = action(transaction.objectStore(name));
            transaction.oncomplete = () => resolve(request.result);
            transaction.onerror = () => reject(transaction.error);
        });
    } finally {
        database.close();
    }
}

async function unrealkitRecords(name) {
    const records = [];
    await unrealkitStore(name, 'readonly', store => {
        const request = store.openCursor();
        request.onsuccess = () => {
            const cursor = request.result;
            if (!cursor) return;
            records.push({key: cursor.key, value: cursor.value});
            cursor.continue();
        };
        return request;
    });
    return records;
}

Module.unrealkitKeep = (path, bytes) => unrealkitStore('storage', 'readwrite', store => store.put(bytes, path));

//! The WasmFS backend of the origin private file system blocks main() in Safari, so /storage is in memory.
async function unrealkitLoadStorage() {
    for (const {key, value} of await unrealkitRecords('storage')) {
        FS.mkdirTree(key.slice(0, key.lastIndexOf('/')));
        FS.writeFile(key, value);
    }
}

async function unrealkitMountHandle(path, kind, handle) {
    if (handle.kind === 'directory') await unrealkitMount(path, path, await unrealkitCollect(handle, unrealkitKinds[kind]));
    else await unrealkitMount('/mnt/files', path, [{path: path.slice('/mnt/files/'.length), file: await handle.getFile()}]);
}

function unrealkitPick(kind, extension) {
    const folder = kind !== 'files';
    if (folder ? window.showDirectoryPicker : window.showOpenFilePicker) {
        return folder
            ? window.showDirectoryPicker({id: 'unrealkit-' + kind, mode: 'read'})
            : window.showOpenFilePicker({id: 'unrealkit-files', types: extension ? [{accept: {'application/octet-stream': ['.' + extension]}}] : undefined})
                  .then(([handle]) => handle);
    }
    return new Promise((resolve, reject) => {
        const input = document.createElement('input');
        input.type = 'file';
        input.webkitdirectory = folder;
        if (extension) input.accept = '.' + extension;
        input.style.display = 'none';
        input.addEventListener('cancel', () => {
            input.remove();
            reject(new DOMException('The picker was closed.', 'AbortError'));
        }, {once: true});
        input.addEventListener('change', () => {
            input.remove();
            resolve(Array.from(input.files));
        }, {once: true});
        document.body.appendChild(input);
        input.click();
    });
}

Module.unrealkitChoose = async (kind, extension) => {
    let picked;
    try {
        picked = await unrealkitPick(kind, extension);
    } catch (error) {
        if (error.name === 'AbortError') return null;
        throw error;
    }
    if (!Array.isArray(picked)) {
        const path = `/mnt/${kind}/${unrealkitMountName(picked.name)}`;
        await unrealkitMountHandle(path, kind, picked);
        await unrealkitStore('mounts', 'readwrite', store => store.put({kind, handle: picked}, path));
        return path;
    }
    if (!picked.length) throw new Error('The picked folder has no files.');
    if (kind === 'files') {
        const name = unrealkitMountName(picked[0].name);
        await unrealkitMount('/mnt/files', `/mnt/files/${name}`, [{path: name, file: picked[0]}]);
        return `/mnt/files/${name}`;
    }
    const root = picked[0].webkitRelativePath.split('/')[0];
    const entries = picked.filter(file => unrealkitMatches(file.name, unrealkitKinds[kind]))
        .map(file => ({path: file.webkitRelativePath.slice(root.length + 1), file}));
    const path = `/mnt/${kind}/${unrealkitMountName(root)}`;
    await unrealkitMount(path, path, entries);
    return path;
};

async function unrealkitRestore(status) {
    const records = (await unrealkitRecords('mounts')).map(record => ({path: record.key, ...record.value}));
    const waiting = [];
    for (const record of records) {
        const permission = await record.handle.queryPermission({mode: 'read'});
        if (permission === 'prompt') waiting.push(record);
        if (permission !== 'granted') continue;
        status.textContent = `Opening ${record.handle.name}…`;
        await unrealkitRestoreRecord(record);
    }
    if (!waiting.length) return;
    const granted = await new Promise(resolve => {
        status.textContent = '';
        const allow = document.createElement('button');
        allow.textContent = 'Allow access to ' + waiting.map(record => record.handle.name).join(', ');
        const skip = document.createElement('button');
        skip.textContent = 'Continue without';
        allow.onclick = async () => {
            const allowed = [];
            for (const record of waiting) {
                try {
                    if (await record.handle.requestPermission({mode: 'read'}) === 'granted') allowed.push(record);
                } catch (error) {
                    console.error('The permission request for ' + record.path + ' failed:', error);
                }
            }
            resolve(allowed);
        };
        skip.onclick = () => resolve([]);
        status.append(allow, skip);
    });
    for (const record of granted) {
        status.textContent = `Opening ${record.handle.name}…`;
        await unrealkitRestoreRecord(record);
    }
}

async function unrealkitRestoreRecord(record) {
    try {
        await unrealkitMountHandle(record.path, record.kind, record.handle);
    } catch (error) {
        console.error('Restoring ' + record.path + ' failed:', error);
        if (error.name === 'NotFoundError') await unrealkitStore('mounts', 'readwrite', store => store.delete(record.path));
    }
}

Module.unrealkitStart = async status => {
    await unrealkitLoadStorage();
    await unrealkitRestore(status);
};
