import flatbuffers

from . import scene_generated as fb
from .product import NAME, VERSION


class ProtocolError(ValueError):
    pass


def text(value):
    return value.decode("utf-8") if value is not None else ""


RECORD_TABLES = {
    fb.Record.Hello: fb.Hello,
    fb.Record.Ready: fb.Ready,
    fb.Record.Begin: fb.Begin,
    fb.Record.Complete: fb.Complete,
    fb.Record.Failed: fb.Failed,
    fb.Record.Image: fb.Image,
    fb.Record.Material: fb.Material,
    fb.Record.Skeleton: fb.Skeleton,
    fb.Record.Mesh: fb.Mesh,
    fb.Record.Light: fb.Light,
    fb.Record.Camera: fb.Camera,
    fb.Record.Animation: fb.Animation,
    fb.Record.Scene: fb.Scene,
    fb.Record.Collection: fb.Collection,
    fb.Record.Object: fb.Object,
    fb.Record.Instances: fb.Instances,
}

RESOURCE_RECORDS = (fb.Record.Image, fb.Record.Material, fb.Record.Skeleton, fb.Record.Mesh, fb.Record.Light, fb.Record.Camera,
                    fb.Record.Instances, fb.Record.Animation, fb.Record.Scene, fb.Record.Collection, fb.Record.Object)


def decode(message):
    if len(message) < 8 or not fb.Envelope.EnvelopeBufferHasIdentifier(message, 0):
        raise ProtocolError("the message is not a scene stream record")
    envelope = fb.Envelope.GetRootAs(message, 0)
    kind = envelope.RecordType()
    table_type = RECORD_TABLES.get(kind)
    union = envelope.Record()
    if table_type is None or union is None:
        raise ProtocolError(f"the app sent the record type {kind}, which Blender does not take")
    record = table_type()
    record.Init(union.Bytes, union.Pos)
    return kind, record


def _envelope(builder, kind, record):
    fb.EnvelopeStart(builder)
    fb.EnvelopeAddRecordType(builder, kind)
    fb.EnvelopeAddRecord(builder, record)
    builder.Finish(fb.EnvelopeEnd(builder), file_identifier=b"VSCN")
    return bytes(builder.Output())


def _diagnostic(builder, diagnostic):
    severity, code, message, subject = diagnostic
    code, message, subject = builder.CreateString(code), builder.CreateString(message), builder.CreateString(subject)
    fb.DiagnosticStart(builder)
    fb.DiagnosticAddSeverity(builder, severity)
    fb.DiagnosticAddCode(builder, code)
    fb.DiagnosticAddMessage(builder, message)
    fb.DiagnosticAddSubject(builder, subject)
    return fb.DiagnosticEnd(builder)


def hello():
    builder = flatbuffers.Builder(48)
    product_version = builder.CreateString(VERSION)
    fb.HelloStart(builder)
    fb.HelloAddVersion(builder, fb.Version.Current)
    fb.HelloAddProductVersion(builder, product_version)
    return _envelope(builder, fb.Record.Hello, fb.HelloEnd(builder))


def ready():
    builder = flatbuffers.Builder(32)
    fb.ReadyStart(builder)
    return _envelope(builder, fb.Record.Ready, fb.ReadyEnd(builder))


def progress(done, total):
    builder = flatbuffers.Builder(48)
    fb.ProgressStart(builder)
    fb.ProgressAddDone(builder, done)
    fb.ProgressAddTotal(builder, total)
    return _envelope(builder, fb.Record.Progress, fb.ProgressEnd(builder))


def finished(diagnostics):
    builder = flatbuffers.Builder(256)
    offsets = [_diagnostic(builder, diagnostic) for diagnostic in diagnostics]
    fb.FinishedStartDiagnosticsVector(builder, len(offsets))
    for offset in reversed(offsets):
        builder.PrependUOffsetTRelative(offset)
    vector = builder.EndVector()
    fb.FinishedStart(builder)
    fb.FinishedAddDiagnostics(builder, vector)
    return _envelope(builder, fb.Record.Finished, fb.FinishedEnd(builder))


def failed(code, message, subject=""):
    builder = flatbuffers.Builder(128)
    diagnostic = _diagnostic(builder, (fb.Severity.Error, code, message, subject))
    fb.FailedStart(builder)
    fb.FailedAddDiagnostic(builder, diagnostic)
    return _envelope(builder, fb.Record.Failed, fb.FailedEnd(builder))


def diagnostics_of(record):
    result = []
    for index in range(record.DiagnosticsLength()):
        diagnostic = record.Diagnostics(index)
        result.append((diagnostic.Severity(), text(diagnostic.Code()), text(diagnostic.Message()), text(diagnostic.Subject())))
    return result


class Session:

    def __init__(self, builder_type):
        self.builder_type = builder_type
        self.state = "hello"
        self.builder = None
        self.title = ""
        self.received = 0
        self.built = 0

    def accept(self, kind, record):
        if self.state == "hello":
            if kind != fb.Record.Hello:
                raise ProtocolError("the first record is not Hello")
            if record.Version() != fb.Version.Current:
                raise ProtocolError(f"the app speaks scene stream version {record.Version()}, the extension "
                                    f"{fb.Version.Current}; install the extension of the app")
            self.state = "begin"
            return ready()
        if kind == fb.Record.Failed and self.state in ("begin", "receiving"):
            self.state = "stopped"
            if self.builder is not None:
                self.builder.finish()
            print(f"[{NAME}] The app stopped the export: {text(record.Diagnostic().Message())}")
            return None
        if self.state == "begin":
            if kind != fb.Record.Begin:
                raise ProtocolError("Begin does not follow Hello")
            self.title = text(record.Title())
            self.builder = self.builder_type(self.title)
            self.state = "receiving"
            return None
        if self.state != "receiving":
            raise ProtocolError(f"a record came after the session ended ({self.state})")
        if kind in RESOURCE_RECORDS:
            self.received += 1
            self.builder.build(kind, record)
            self.built += 1
            return None
        if kind == fb.Record.Complete:
            self.state = "finished"
            return finished(diagnostics_of(record) + self.builder.finish())
        raise ProtocolError(f"the app sent the record type {kind} during the session")
