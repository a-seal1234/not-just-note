#!/usr/bin/env python3
"""Build the PDF export fixtures used by PdfExporterTest.

Everything here is deterministic and self-contained except the files produced by Ghostscript
(the *-objstm-*.pdf and ex-encrypted.pdf fixtures), which exist precisely because Ghostscript is
a real third party producer: its output locates objects through a cross reference stream and packs
dictionaries into compressed object streams, which is the class of PDF the exporter had to learn.

Run from anywhere:

    python3 krita/plugins/extensions/pdfio/tests/data/ex-make-fixtures.py

It rewrites the fixtures next to itself. Ghostscript is optional; without it the plain fixtures
are still rebuilt and the script reports which ones it skipped.
"""
import os
import shutil
import subprocess
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
NL = b"\n"
FS = b"\xe2\xe3\xcf\xd3"  # the binary marker comment every PDF starts with


def stream_object(dictionary, data):
    return (b"<<" + dictionary + b" /Length " + str(len(data)).encode() + b">>" + NL
            + b"stream" + NL + data + NL + b"endstream")


def text_stream(box, heading, body):
    """Text placed inside the page's own box: the offset MediaBox page and the short landscape
    pages would otherwise carry text nothing can render."""
    tx = box[0] + 22
    ty = box[3] - 42
    return (("BT /F1 24 Tf %d %d Td (%s) Tj ET\n" % (tx, ty, heading)).encode("latin-1")
            + ("BT /F1 12 Tf %d %d Td (%s) Tj ET\n" % (tx, ty - 40, body)).encode("latin-1"))


def classic_pdf(pages):
    """A PDF 1.4 classic file.

    pages is a list of (media_box, rotate, heading, body). Page objects come first at
    numbers 3, 5, 7, ... and their content streams at 4, 6, 8, ..., so tests can name them.
    """
    font_object = 2 + 2 * len(pages) + 1
    objects = []
    kids = " ".join("%d 0 R" % (3 + 2 * i) for i in range(len(pages)))
    objects.append(b"<< /Type /Catalog /Pages 2 0 R >>")
    objects.append(("<< /Type /Pages /Kids [%s] /Count %d >>" % (kids, len(pages))).encode())
    for i, (box, rotate, heading, body) in enumerate(pages):
        content_number = 4 + 2 * i
        rotation = (" /Rotate %d" % rotate) if rotate else ""
        objects.append((
            "<< /Type /Page /Parent 2 0 R /MediaBox [%d %d %d %d]%s /Contents %d 0 R "
            "/Resources << /Font << /F1 %d 0 R >> >> >>"
            % (box[0], box[1], box[2], box[3], rotation, content_number, font_object)
        ).encode())
        objects.append(stream_object(b"", text_stream(box, heading, body)))
    objects.append(b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>")

    out = [b"%PDF-1.4", b"%" + FS]
    offsets = []
    for number, body in enumerate(objects, start=1):
        offsets.append(sum(len(line) + 1 for line in out))
        out.append(b"%d 0 obj" % number)
        out.append(body)
        out.append(b"endobj")
    xref_at = sum(len(line) + 1 for line in out)
    out.append(b"xref")
    out.append(b"0 %d" % (len(objects) + 1))
    out.append(b"0000000000 65535 f ")
    for offset in offsets:
        out.append(b"%010d 00000 n " % offset)
    out.append(b"trailer")
    out.append(b"<< /Size %d /Root 1 0 R >>" % (len(objects) + 1))
    out.append(b"startxref")
    out.append(b"%d" % xref_at)
    out.append(b"%%EOF")
    return NL.join(out) + NL


ROTATIONS = [
    ((0, 0, 595, 842), 0, "Rotation zero", "Portrait, no rotation."),
    ((0, 0, 595, 842), 90, "Rotation ninety", "Quarter turn clockwise."),
    ((0, 0, 595, 842), 180, "Rotation one eighty", "Upside down."),
    ((0, 0, 595, 842), 270, "Rotation two seventy", "Three quarter turn."),
    ((50, 30, 350, 330), 0, "Offset media box", "MediaBox does not start at the origin."),
]

# Mixed sizes and rotations for the many page notebook: A4 portrait, A5 landscape (rotated),
# Letter portrait, A4 landscape (rotated the other way).
MANY_GEOMETRY = [
    ((0, 0, 595, 842), 0),
    ((0, 0, 595, 420), 90),
    ((0, 0, 612, 792), 0),
    ((0, 0, 420, 595), 270),
]


def manypage_pdf(count):
    pages = []
    for i in range(count):
        box, rotate = MANY_GEOMETRY[i % len(MANY_GEOMETRY)]
        pages.append((box, rotate, "Page %d heading" % (i + 1),
                      "Body line one for page %d." % (i + 1)))
    return classic_pdf(pages)


def objstm_predictor_pdf(filter_name=b"FlateDecode"):
    """Hand-built PDF 1.5: catalog, page tree and both pages inside one object stream, located
    through a cross reference stream compressed with the PNG Up predictor.

    Ghostscript on this machine happens to emit no predictor, and browsers do, so this fixture is
    what keeps the predictor path honest. With filter_name=b"LZWDecode" the same file declares a
    filter no reader has to support, which is the refusal fixture.
    """
    catalog = b"<< /Type /Catalog /Pages 2 0 R >>"
    page_tree = b"<< /Type /Pages /Kids [3 0 R 4 0 R] /Count 2 >>"
    page_one = (b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595 842] /Contents 5 0 R "
                b"/Resources << /Font << /F1 7 0 R >> >> >>")
    page_two = (b"<< /Type /Page /Parent 2 0 R /MediaBox [50 30 350 330] /Rotate 180 "
                b"/Contents 6 0 R /Resources << /Font << /F1 7 0 R >> >> >>")
    font = b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>"

    header = b""
    bodies = b""
    for number, body in enumerate([catalog, page_tree, page_one, page_two], start=1):
        header += b"%d %d " % (number, len(bodies))
        bodies += body + NL
    first = len(header)
    objstm_payload = zlib.compress(header + bodies, 9)
    objstm = stream_object(
        b"/Type /ObjStm /N 4 /First %d /Filter /FlateDecode" % first, objstm_payload)

    out = [b"%PDF-1.5", b"%" + FS]
    offsets = {}

    def add(number, body):
        offsets[number] = sum(len(line) + 1 for line in out)
        out.append(b"%d 0 obj" % number)
        out.append(body)
        out.append(b"endobj")

    add(5, stream_object(b"", text_stream((0, 0, 595, 842), "Predictor page one",
                                          "Inside an object stream.")))
    add(6, stream_object(b"", text_stream((50, 30, 350, 330), "Predictor page two",
                                          "Offset box, rotated.")))
    add(7, font)
    add(8, objstm)

    xref_at = sum(len(line) + 1 for line in out)

    def entry(kind, field_one, field_two):
        return bytes([kind]) + field_one.to_bytes(2, "big") + field_two.to_bytes(2, "big")

    rows = [
        entry(0, 0, 65535),          # 0 free
        entry(2, 8, 0),              # 1 catalog, object stream 8, index 0
        entry(2, 8, 1),              # 2 page tree
        entry(2, 8, 2),              # 3 page one
        entry(2, 8, 3),              # 4 page two
        entry(1, offsets[5], 0),
        entry(1, offsets[6], 0),
        entry(1, offsets[7], 0),
        entry(1, offsets[8], 0),
        entry(1, xref_at, 0),        # 9 the xref stream itself
    ]
    raw = b"".join(rows)

    # PNG Up predictor (type 2), one row per xref entry, five bytes wide.
    encoded = b""
    previous = bytes(5)
    for i in range(0, len(raw), 5):
        row = raw[i:i + 5]
        encoded += bytes([2]) + bytes(((row[j] - previous[j]) & 0xFF) for j in range(5))
        previous = row
    compressed = zlib.compress(encoded, 9)

    add(9, stream_object(
        b"/Type /XRef /Size 10 /Root 1 0 R /W [1 2 2] /Index [0 10]"
        b" /Filter /" + filter_name + b" /DecodeParms << /Predictor 12 /Columns 5 >>",
        compressed))

    out.append(b"startxref")
    out.append(b"%d" % xref_at)
    out.append(b"%%EOF")
    return NL.join(out) + NL


BROWSER_HTML = """<!doctype html><html><head><meta charset="utf-8"><style>
@page { size: A4; margin: 40px; }
.page { page-break-after: always; }
h1 { font-family: Helvetica, sans-serif; font-size: 24pt; }
p  { font-family: Helvetica, sans-serif; font-size: 12pt; }
</style></head><body>
<div class="page"><h1>Browser page one</h1><p>Printed by headless Chromium.</p></div>
<div class="page"><h1>Browser page two</h1><p>Second page of the browser fixture.</p></div>
<div class="page"><h1>Browser page three</h1><p>Third page of the browser fixture.</p></div>
</body></html>
"""


def chromium(name):
    """A real browser print: Skia/PDF writes its own object layout, compact dictionaries and a
    trailer with /Info, which is exactly the kind of file an export has to survive."""
    browser = shutil.which("chromium") or shutil.which("chromium-browser")
    if not browser:
        print("skipped %s: no Chromium on this machine" % name)
        return
    html_path = os.path.join(HERE, ".ex-browser-source.html")
    with open(html_path, "w", encoding="utf-8") as handle:
        handle.write(BROWSER_HTML)
    target = os.path.join(HERE, name)
    command = [browser, "--headless=new", "--disable-gpu", "--no-sandbox",
               "--no-pdf-header-footer", "--user-data-dir=/tmp/ex-pdfio-chromium",
               "--print-to-pdf=" + target, "file://" + html_path]
    result = subprocess.run(command, capture_output=True)
    os.remove(html_path)
    if result.returncode != 0 or not os.path.exists(target):
        print("skipped %s: Chromium failed: %s" % (name, result.stderr[-200:].decode(errors="replace")))
        return
    print("wrote %s (%d bytes) with Chromium" % (name, os.path.getsize(target)))


def indirect_contents_pdf():
    """Three pages exercising the two content-list shapes and an unreadable /MediaBox.

    Page one's /Contents is an indirect reference to an array (object 9), which real producers write
    and which the update has to splice flat instead of nesting. Page two's /Contents is the ordinary
    single indirect stream. Page three's /MediaBox is itself an indirect array, so the exporter's own
    reader cannot parse it and has to fall back to the size the notebook recorded -- and the page's
    content still has to survive that. Object numbers are stable because the test names them:
    3 page one, 4 and 5 its two content streams, 6 page two, 7 its stream, 8 page three, 9 the
    /Contents array, 10 the indirect /MediaBox, 11 page three's stream, 12 the font.
    """
    box = (0, 0, 595, 842)
    objects = [
        b"<< /Type /Catalog /Pages 2 0 R >>",
        b"<< /Type /Pages /Kids [3 0 R 6 0 R 8 0 R] /Count 3 >>",
        b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595 842] /Contents 9 0 R "
        b"/Resources << /Font << /F1 12 0 R >> >> >>",
        stream_object(b"", text_stream(box, "Indirect contents page one", "First half of page one.")),
        stream_object(b"", b"BT /F1 12 Tf 22 700 Td (Second half of page one.) Tj ET\n"),
        b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595 842] /Contents 7 0 R "
        b"/Resources << /Font << /F1 12 0 R >> >> >>",
        stream_object(b"", text_stream(box, "Direct page two", "The ordinary case.")),
        b"<< /Type /Page /Parent 2 0 R /MediaBox 10 0 R /Contents 11 0 R "
        b"/Resources << /Font << /F1 12 0 R >> >> >>",
        b"[4 0 R 5 0 R]",
        b"[0 0 595 842]",
        stream_object(b"", text_stream(box, "Indirect media box page three", "Box on another object.")),
        b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
    ]

    out = [b"%PDF-1.4", b"%" + FS]
    offsets = []
    for number, body in enumerate(objects, start=1):
        offsets.append(sum(len(line) + 1 for line in out))
        out.append(b"%d 0 obj" % number)
        out.append(body)
        out.append(b"endobj")
    xref_at = sum(len(line) + 1 for line in out)
    out.append(b"xref")
    out.append(b"0 %d" % (len(objects) + 1))
    out.append(b"0000000000 65535 f ")
    for offset in offsets:
        out.append(b"%010d 00000 n " % offset)
    out.append(b"trailer")
    out.append(b"<< /Size %d /Root 1 0 R >>" % (len(objects) + 1))
    out.append(b"startxref")
    out.append(b"%d" % xref_at)
    out.append(b"%%EOF")
    return NL.join(out) + NL


def mediabox_cases_pdf():
    """Five pages, one per /MediaBox shape the reader has to survive.

    Page 1 a direct array, page 2 an indirect reference, page 3 a direct array with /Rotate 90,
    page 4 an indirect reference with /Rotate 90 (a fallback has to give the box back in PAGE space,
    not the displayed size), page 5 no /MediaBox anywhere. Object numbers are stable because the test
    names them: 3, 5, 7, 9, 11 are the pages, 4, 6, 8, 10, 15 their content streams, 12 and 13 the
    indirect boxes, 14 the font.
    """
    box = (0, 0, 595, 842)
    objects = [
        b"<< /Type /Catalog /Pages 2 0 R >>",
        b"<< /Type /Pages /Kids [3 0 R 5 0 R 7 0 R 9 0 R 11 0 R] /Count 5 >>",
        b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595 842] /Contents 4 0 R "
        b"/Resources << /Font << /F1 14 0 R >> >> >>",
        stream_object(b"", text_stream(box, "Direct media box", "Read straight from the page.")),
        b"<< /Type /Page /Parent 2 0 R /MediaBox 12 0 R /Contents 6 0 R "
        b"/Resources << /Font << /F1 14 0 R >> >> >>",
        stream_object(b"", text_stream(box, "Indirect media box", "The box is another object.")),
        b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595 842] /Rotate 90 /Contents 8 0 R "
        b"/Resources << /Font << /F1 14 0 R >> >> >>",
        stream_object(b"", text_stream(box, "Rotated direct box", "A quarter turn clockwise.")),
        b"<< /Type /Page /Parent 2 0 R /MediaBox 13 0 R /Rotate 90 /Contents 10 0 R "
        b"/Resources << /Font << /F1 14 0 R >> >> >>",
        stream_object(b"", text_stream(box, "Rotated indirect box", "Displayed size is swapped.")),
        b"<< /Type /Page /Parent 2 0 R /Rotate 270 /Contents 15 0 R "
        b"/Resources << /Font << /F1 14 0 R >> >> >>",
        b"[0 0 595 842]",
        b"[0 0 595 842]",
        b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
        # Letter, not A4: with no /MediaBox to read, a reader falls back to Letter, and text placed
        # for an A4 box would be clipped away before it could prove anything.
        stream_object(b"", text_stream((0, 0, 612, 792), "Missing media box",
                                       "Nothing to read at all.")),
    ]

    out = [b"%PDF-1.4", b"%" + FS]
    offsets = []
    for number, body in enumerate(objects, start=1):
        offsets.append(sum(len(line) + 1 for line in out))
        out.append(b"%d 0 obj" % number)
        out.append(body)
        out.append(b"endobj")
    xref_at = sum(len(line) + 1 for line in out)
    out.append(b"xref")
    out.append(b"0 %d" % (len(objects) + 1))
    out.append(b"0000000000 65535 f ")
    for offset in offsets:
        out.append(b"%010d 00000 n " % offset)
    out.append(b"trailer")
    out.append(b"<< /Size %d /Root 1 0 R >>" % (len(objects) + 1))
    out.append(b"startxref")
    out.append(b"%d" % xref_at)
    out.append(b"%%EOF")
    return NL.join(out) + NL


def write(name, data):
    path = os.path.join(HERE, name)
    with open(path, "wb") as handle:
        handle.write(data)
    print("wrote %s (%d bytes)" % (name, len(data)))


def ghostscript(name, source, extra):
    gs = shutil.which("gs")
    if not gs:
        print("skipped %s: no Ghostscript on this machine" % name)
        return
    target = os.path.join(HERE, name)
    command = [gs, "-q", "-dNOPAUSE", "-dBATCH", "-sDEVICE=pdfwrite"] + extra + \
              ["-o", target, os.path.join(HERE, source)]
    subprocess.run(command, check=True)
    print("wrote %s (%d bytes) with Ghostscript" % (name, os.path.getsize(target)))


def main():
    write("ex-rotations.pdf", classic_pdf(ROTATIONS))
    write("ex-manypage-50.pdf", manypage_pdf(50))
    write("ex-objstm-predictor.pdf", objstm_predictor_pdf())
    write("ex-objstm-badfilter.pdf", objstm_predictor_pdf(b"LZWDecode"))
    write("ex-indirect-contents.pdf", indirect_contents_pdf())
    write("ex-mediabox-cases.pdf", mediabox_cases_pdf())

    # Real third party producers: PDF 1.5+ object streams and cross reference streams.
    ghostscript("ex-objstm-rotations.pdf", "ex-rotations.pdf", ["-dCompatibilityLevel=1.5"])
    ghostscript("ex-objstm-50p.pdf", "ex-manypage-50.pdf", ["-dCompatibilityLevel=1.5"])
    ghostscript("ex-encrypted.pdf", "ex-rotations.pdf",
                ["-sOwnerPassword=perfectnote", "-sUserPassword=perfectnote",
                 "-dEncryptionR=3", "-dKeyLength=128"])
    chromium("ex-browser-3p.pdf")
    return 0


if __name__ == "__main__":
    sys.exit(main())
