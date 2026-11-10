addToLibrary({
    unrealkit_keep_file__proxy: 'sync',
    unrealkit_keep_file__deps: ['$UTF8ToString'],
    unrealkit_keep_file__sig: 'vppp',
    unrealkit_keep_file: (path, bytes, size) => {
        Module.unrealkitKeep(UTF8ToString(path), HEAPU8.slice(bytes, bytes + size));
    },
});
