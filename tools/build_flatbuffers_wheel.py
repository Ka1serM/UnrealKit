import base64
import hashlib
import pathlib
import re
import sys
import zipfile


def record_line(path, data):
    digest = base64.urlsafe_b64encode(hashlib.sha256(data).digest()).rstrip(b"=").decode()
    return f"{path},sha256={digest},{len(data)}"


def main():
    python, wheel = (pathlib.Path(argument) for argument in sys.argv[1:3])
    version = re.search(r'__version__ = "([^"]+)"', (python / "flatbuffers" / "_version.py").read_text()).group(1)
    expected = f"flatbuffers-{version}-py3-none-any.whl"
    if wheel.name != expected: sys.exit(f"The FlatBuffers runtime is {version}, so the wheel must be {expected}, not {wheel.name}.")
    dist_info = f"flatbuffers-{version}.dist-info"
    files = {path.relative_to(python).as_posix(): path.read_bytes()
             for path in sorted((python / "flatbuffers").glob("*.py"))}
    files[f"{dist_info}/METADATA"] = f"Metadata-Version: 2.1\nName: flatbuffers\nVersion: {version}\n".encode()
    files[f"{dist_info}/WHEEL"] = b"Wheel-Version: 1.0\nGenerator: verylia\nRoot-Is-Purelib: true\nTag: py3-none-any\n"
    record = [record_line(path, data) for path, data in files.items()] + [f"{dist_info}/RECORD,,"]
    files[f"{dist_info}/RECORD"] = ("\n".join(record) + "\n").encode()
    wheel.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(wheel, "w", zipfile.ZIP_DEFLATED) as archive:
        for path, data in files.items():
            archive.writestr(path, data)


main()
