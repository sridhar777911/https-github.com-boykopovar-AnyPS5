import argparse
import html
import re
from pathlib import Path
from urllib.parse import unquote, urlsplit


INLINE_LINK = re.compile(r"!?\[[^\]\n]*\]\(\s*(<[^>\n]+>|[^\s)]+)(?:\s+[\"'][^\n]*?[\"'])?\s*\)")
REFERENCE = re.compile(r"^\s{0,3}\[[^\]]+\]:\s*(<[^>]+>|\S+)")
HEADING = re.compile(r"^\s{0,3}#{1,6}\s+(.+?)\s*#*\s*$")


def prose_lines(text):
    fence = None
    in_comment = False
    for number, line in enumerate(text.splitlines(), 1):
        marker = re.match(r"^\s{0,3}(`{3,}|~{3,})", line)
        if fence:
            if marker and marker.group(1)[0] == fence[0] and len(marker.group(1)) >= len(fence) and not line[marker.end():].strip():
                fence = None
            continue
        if marker:
            fence = marker.group(1)
            continue
        visible = []
        rest = line
        while rest:
            if in_comment:
                _, end, rest = rest.partition("-->")
                if not end:
                    break
                in_comment = False
            else:
                before, start, rest = rest.partition("<!--")
                visible.append(before)
                if not start:
                    break
                in_comment = True
        yield number, "".join(visible)


def heading_ids(text):
    result = set()
    for _, line in prose_lines(text):
        match = HEADING.match(line)
        if not match:
            continue
        heading = re.sub(r"<[^>]*>", "", html.unescape(match.group(1)))
        heading = re.sub(r"\[([^\]]+)\]\([^)]*\)", r"\1", heading)
        slug = re.sub(r"[^\w\- ]", "", heading.lower()).replace(" ", "-")
        candidate = slug
        suffix = 0
        while candidate in result:
            suffix += 1
            candidate = f"{slug}-{suffix}"
        result.add(candidate)
    return result


def documentation_files(root):
    result = set(root.glob("*.md"))
    for directory in ("docs", ".github"):
        result.update((root / directory).rglob("*.md"))
    for directory in ("core", "tools"):
        result.update((root / directory).rglob("AGENTS.md"))
    return sorted(path for path in result if path.is_file())


def check_document(root, path):
    findings = []
    try:
        content = path.read_text(encoding="utf-8")
    except UnicodeError:
        return [(path, 0, "Document is not valid UTF-8")]
    for number, line in prose_lines(content):
        line = re.sub(r"(`+).*?\1", "", line)
        targets = [match.group(1) for match in INLINE_LINK.finditer(line)]
        reference = REFERENCE.match(line)
        if reference:
            targets.append(reference.group(1))
        for raw in targets:
            target = html.unescape(raw.strip("<>"))
            parts = urlsplit(target)
            if parts.scheme or parts.netloc:
                continue
            local_path = unquote(parts.path)
            resolved = (root / local_path.lstrip("/") if local_path.startswith("/") else path.parent / local_path).resolve() if local_path else path
            if not resolved.is_relative_to(root):
                findings.append((path, number, f"Link escapes repository: {target}"))
            elif not resolved.exists():
                findings.append((path, number, f"Missing local target: {target}"))
            elif parts.fragment and resolved.suffix.lower() == ".md":
                try:
                    anchors = heading_ids(resolved.read_text(encoding="utf-8"))
                except UnicodeError:
                    findings.append((path, number, f"Target is not valid UTF-8: {target}"))
                    continue
                if unquote(parts.fragment) not in anchors:
                    findings.append((path, number, f"Missing Markdown heading: {target}"))
    return findings


def check(root):
    root = root.resolve()
    files = documentation_files(root)
    findings = [finding for path in files for finding in check_document(root, path)]
    return files, findings


def main():
    parser = argparse.ArgumentParser(description="Check current maintained Markdown local links and heading fragments without network access.")
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    args = parser.parse_args()
    root = args.root.resolve()
    if not (root / "README.md").is_file():
        parser.error("--root must contain README.md")
    files, findings = check(root)
    for path, line, detail in findings:
        print(f"{path.relative_to(root)}:{line}: {detail}")
    print(f"Checked {len(files)} Markdown files; {len(findings)} errors.")
    return bool(findings)


if __name__ == "__main__":
    raise SystemExit(main())
