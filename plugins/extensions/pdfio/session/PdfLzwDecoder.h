/*
 *  SPDX-FileCopyrightText: 2026 PerfectNote contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef PDFLZWDECODER_H
#define PDFLZWDECODER_H

#include <QByteArray>
#include <QString>

#include <algorithm>

/**
 * PDF LZWDecode (ISO 32000-1, 7.4.4.2), header-only so the exporter and the assembler decode a
 * structural stream through one implementation instead of two that drift apart.
 *
 * LZW is a single filter, not a whole pipeline: the caller still applies any /DecodeParms predictor
 * after this returns, exactly as it does for FlateDecode. The caller also supplies /EarlyChange,
 * because PDF gives that parameter a default of 1 and this helper must not silently substitute one
 * value for the other.
 *
 * Failing is part of the contract. A malformed stream -- an unassigned code, a code that runs off
 * the end of the data, an /EarlyChange outside 0..1 -- returns false with a reason instead of
 * whatever bytes happened to decode, and the output is capped so a corrupt length or a
 * decompression bomb cannot exhaust memory before anyone notices.
 */
namespace PdfLzwDecoder {

/// The most a decoded structural stream may become. A real xref or object stream is a few MiB at
/// most; 64 MiB leaves headroom while still bounding one stream to a small share of the process.
constexpr int maxOutputBytes = 64 * 1024 * 1024;

/// Decodes input as a PDF LZWDecode stream. earlyChange is the stream's /EarlyChange value and
/// must be 0 or 1. On success output holds the decoded bytes and true is returned. On failure
/// output is left empty, why (when non-null) explains the fault, and false is returned.
inline bool decode(const QByteArray &input, int earlyChange, QByteArray *output, QString *why)
{
    const auto fail = [why](const QString &message) {
        if (why) {
            *why = message;
        }
        return false;
    };

    if (!output) {
        return fail(QStringLiteral("the LZW stream has no output buffer"));
    }
    output->clear();

    if (earlyChange != 0 && earlyChange != 1) {
        return fail(QStringLiteral("the LZW stream declares /EarlyChange %1; only 0 and 1 are "
                                   "defined").arg(earlyChange));
    }
    if (input.isEmpty()) {
        return fail(QStringLiteral("the LZW stream is empty"));
    }

    // Every string is stored as its last code and its suffix byte, so the table needs 4096 pairs of
    // small values rather than 4096 heap strings.
    constexpr int tableSize = 4096;
    int prefix[tableSize];
    unsigned char suffix[tableSize];
    for (int i = 0; i < 256; ++i) {
        prefix[i] = -1;
        suffix[i] = static_cast<unsigned char>(i);
    }

    int nextCode = 258;
    int codeLength = 9;
    int position = 0;
    quint32 cache = 0;
    int cachedBits = 0;

    // Reads bits MSB-first, or returns false when the input has run out.
    const auto readBits = [&](int bits, int *code) {
        while (cachedBits < bits) {
            if (position >= input.size()) {
                return false;
            }
            cache = (cache << 8) | static_cast<quint8>(input.at(position++));
            cachedBits += 8;
        }
        cachedBits -= bits;
        *code = int((cache >> cachedBits) & ((1u << bits) - 1u));
        return true;
    };

    // Expands a code into sequence, in stream order, and reports its first byte.
    QByteArray sequence;
    const auto expand = [&](int code, int *first) {
        sequence.clear();
        int current = code;
        int walked = 0;
        while (current != -1) {
            if (current < 0 || current >= nextCode || ++walked > tableSize) {
                return false;
            }
            sequence.append(char(suffix[current]));
            current = prefix[current];
        }
        std::reverse(sequence.begin(), sequence.end());
        *first = sequence.isEmpty() ? 0 : static_cast<unsigned char>(sequence.at(0));
        return true;
    };

    QByteArray decoded;
    const int room = int(qMin<qint64>(qint64(input.size()) * 3, maxOutputBytes));
    decoded.reserve(room);

    bool hasPrevious = false;
    int previousCode = -1;

    while (true) {
        int code = 0;
        if (!readBits(codeLength, &code)) {
            return fail(QStringLiteral("the LZW stream ends before its end-of-data code"));
        }
        if (code == 256) {
            nextCode = 258;
            codeLength = 9;
            hasPrevious = false;
            previousCode = -1;
            continue;
        }
        if (code == 257) {
            *output = decoded;
            return true;
        }

        int first = 0;
        if (code < nextCode) {
            if (!expand(code, &first)) {
                return fail(QStringLiteral("the LZW stream's code table is corrupt"));
            }
        } else if (code == nextCode && hasPrevious) {
            // The KwKwK case: the code being read is the one the decoder is about to define, so
            // its string is the previous string plus its own first byte.
            if (!expand(previousCode, &first)) {
                return fail(QStringLiteral("the LZW stream's code table is corrupt"));
            }
            sequence.append(char(first));
        } else {
            return fail(QStringLiteral("the LZW stream uses the unassigned code %1 (the next code "
                                       "is %2)").arg(code).arg(nextCode));
        }

        if (hasPrevious && nextCode < tableSize) {
            prefix[nextCode] = previousCode;
            suffix[nextCode] = static_cast<unsigned char>(first);
            ++nextCode;
            if (nextCode + earlyChange == (1 << codeLength) && codeLength < 12) {
                ++codeLength;
            }
        }

        if (decoded.size() + sequence.size() > maxOutputBytes) {
            return fail(QStringLiteral("the LZW stream decodes to more than %1 MiB")
                            .arg(maxOutputBytes / (1024 * 1024)));
        }
        decoded += sequence;
        previousCode = code;
        hasPrevious = true;
    }
}

} // namespace PdfLzwDecoder

#endif // PDFLZWDECODER_H
