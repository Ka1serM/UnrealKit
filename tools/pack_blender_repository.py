import hashlib
import json
import pathlib
import shutil
import sys
import tomllib
import zipfile


def extension_files(source):
    yield source / "blender_manifest.toml"
    yield from sorted(source.glob("*.py"))
    yield from sorted(path for path in (source / "wheels").rglob("*") if path.is_file())


def pack(source, repository):
    manifest = tomllib.loads((source / "blender_manifest.toml").read_text(encoding="utf-8"))
    archive = repository / f"{manifest['id']}.zip"
    with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as package:
        for path in extension_files(source):
            package.write(path, path.relative_to(source).as_posix())
    data = archive.read_bytes()
    return manifest | {
        "archive_url": f"./{archive.name}",
        "archive_size": len(data),
        "archive_hash": f"sha256:{hashlib.sha256(data).hexdigest()}",
    }


def main():
    repository, *sources = (pathlib.Path(argument) for argument in sys.argv[1:])
    if not sources:
        sys.exit("Give the repository folder and at least one staged extension folder.")
    shutil.rmtree(repository, ignore_errors=True)
    repository.mkdir(parents=True)
    index = {"version": "v1", "blocklist": [], "data": [pack(source, repository) for source in sources]}
    (repository / "index.json").write_text(json.dumps(index, indent=2), encoding="utf-8")

main()
