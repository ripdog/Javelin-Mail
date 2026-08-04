#!/usr/bin/env python3
"""Generate Javelin's immutable Firefox translation-model manifest.

The application never queries Remote Settings at runtime. Maintainers run this tool, review the
stable diff, and commit the resulting manifest together with any engine compatibility update.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
import urllib.request
from collections import defaultdict
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Iterable

COLLECTION_URL = (
    "https://firefox.settings.services.mozilla.com/v1/buckets/main/collections/"
    "translations-models-v2"
)
ATTACHMENT_BASE_URL = "https://firefox-settings-attachments.cdn.mozilla.net/"
ENGINE_COMMIT = "4732dc947bc952abb019aabfe5582006d4fc3337"
ENGINE_VERSION = "v0.6.0"
STABLE_VERSION = re.compile(r"^[0-9]+(?:\.[0-9]+)*$")
SHA256 = re.compile(r"^[0-9a-f]{64}$")
REQUIRED_COMMON = frozenset({"model", "lex"})
KNOWN_TYPES = frozenset({"model", "lex", "vocab", "srcvocab", "trgvocab"})
ARCHITECTURE_ORDER = ("base-memory", "base")
INSTALLED_NAMES = {
    "model": "model.bin",
    "lex": "lex.bin",
    "vocab": "vocab.spm",
    "srcvocab": "srcvocab.spm",
    "trgvocab": "trgvocab.spm",
}


@dataclass(frozen=True)
class Candidate:
    source: str
    target: str
    version: str
    architecture: str
    records: tuple[dict[str, Any], ...]


def fetch_json(url: str) -> dict[str, Any]:
    request = urllib.request.Request(url, headers={"User-Agent": "Javelin-Mail manifest generator"})
    with urllib.request.urlopen(request, timeout=60) as response:
        return json.load(response)


def version_key(value: str) -> tuple[int, ...]:
    return tuple(int(part) for part in value.split("."))


def complete(records: Iterable[dict[str, Any]]) -> bool:
    file_types = {record.get("fileType") for record in records}
    return REQUIRED_COMMON <= file_types and (
        "vocab" in file_types or {"srcvocab", "trgvocab"} <= file_types
    )


def choose_candidates(records: list[dict[str, Any]]) -> list[Candidate]:
    grouped: dict[tuple[str, str, str, str], list[dict[str, Any]]] = defaultdict(list)
    for record in records:
        version = str(record.get("version", ""))
        architecture = str(record.get("architecture", ""))
        file_type = str(record.get("fileType", ""))
        if not STABLE_VERSION.fullmatch(version):
            continue
        if architecture not in ARCHITECTURE_ORDER or file_type not in KNOWN_TYPES:
            continue
        grouped[
            (
                str(record.get("sourceLanguage", "")),
                str(record.get("targetLanguage", "")),
                version,
                architecture,
            )
        ].append(record)

    directions: dict[tuple[str, str], list[Candidate]] = defaultdict(list)
    for (source, target, version, architecture), direction_records in grouped.items():
        if source and target and complete(direction_records):
            directions[(source, target)].append(
                Candidate(source, target, version, architecture, tuple(direction_records))
            )

    selected: list[Candidate] = []
    for candidates in directions.values():
        versions = sorted({candidate.version for candidate in candidates}, key=version_key, reverse=True)
        for version in versions:
            same_version = [candidate for candidate in candidates if candidate.version == version]
            choice = next(
                (
                    candidate
                    for architecture in ARCHITECTURE_ORDER
                    for candidate in same_version
                    if candidate.architecture == architecture
                ),
                None,
            )
            if choice is not None:
                selected.append(choice)
                break
    return sorted(selected, key=lambda candidate: (candidate.source, candidate.target))


def validate_record(record: dict[str, Any]) -> None:
    attachment = record.get("attachment")
    if not isinstance(attachment, dict):
        raise ValueError(f"record {record.get('id')} has no attachment")
    location = attachment.get("location")
    if not isinstance(location, str) or not location or location.startswith(("http://", "https://")):
        raise ValueError(f"record {record.get('id')} has an invalid attachment location")
    if attachment.get("mimetype") != "application/zstd":
        raise ValueError(f"record {record.get('id')} is not a zstd attachment")
    for field, source in (
        ("compressedSha256", attachment.get("hash")),
        ("decompressedSha256", record.get("decompressedHash")),
    ):
        if not isinstance(source, str) or not SHA256.fullmatch(source.lower()):
            raise ValueError(f"record {record.get('id')} has an invalid {field}")
    for field, source in (
        ("compressedSize", attachment.get("size")),
        ("decompressedSize", record.get("decompressedSize")),
    ):
        if not isinstance(source, int) or source <= 0:
            raise ValueError(f"record {record.get('id')} has an invalid {field}")


def manifest_file(record: dict[str, Any]) -> dict[str, Any]:
    validate_record(record)
    attachment = record["attachment"]
    file_type = record["fileType"]
    return {
        "type": file_type,
        "url": ATTACHMENT_BASE_URL + attachment["location"],
        "compression": "zstd",
        "compressedSize": attachment["size"],
        "compressedSha256": attachment["hash"].lower(),
        "decompressedSize": record["decompressedSize"],
        "decompressedSha256": record["decompressedHash"].lower(),
        "installedName": INSTALLED_NAMES[file_type],
    }


def build_manifest(collection: dict[str, Any], records: list[dict[str, Any]]) -> dict[str, Any]:
    collection_data = collection.get("data", {})
    revision = collection_data.get("last_modified")
    if not isinstance(revision, int):
        revision = max(int(record.get("last_modified", 0)) for record in records)

    directions = []
    seen: set[tuple[str, str]] = set()
    for candidate in choose_candidates(records):
        key = (candidate.source, candidate.target)
        if key in seen:
            raise ValueError(f"duplicate direction {candidate.source}->{candidate.target}")
        seen.add(key)
        files = sorted(
            (manifest_file(record) for record in candidate.records),
            key=lambda item: (item["type"], item["installedName"]),
        )
        directions.append(
            {
                "source": candidate.source.replace("_", "-"),
                "target": candidate.target.replace("_", "-"),
                "mozillaSource": candidate.source,
                "mozillaTarget": candidate.target,
                "modelVersion": candidate.version,
                "architecture": candidate.architecture,
                "files": files,
                "licenseFiles": ["MPL-2.0.txt"],
            }
        )

    return {
        "schemaVersion": 1,
        "manifestRevision": f"mozilla-remote-settings-{revision}",
        "engine": {
            "name": "bergamot",
            "version": ENGINE_VERSION,
            "sourceCommit": ENGINE_COMMIT,
        },
        "directions": directions,
    }


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--output",
        type=Path,
        default=Path("res/models/translations/manifest-v1.json"),
    )
    parser.add_argument("--collection-url", default=COLLECTION_URL)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    collection = fetch_json(args.collection_url)
    records_payload = fetch_json(args.collection_url.rstrip("/") + "/records")
    records = records_payload.get("data")
    if not isinstance(records, list) or not records:
        raise ValueError("Remote Settings returned no model records")
    manifest = build_manifest(collection, records)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(manifest, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    print(f"wrote {len(manifest['directions'])} directions to {args.output}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, json.JSONDecodeError) as error:
        print(f"error: {error}", file=sys.stderr)
        raise SystemExit(1)
