#!/usr/bin/env python3
# checks every catalog in res/lang against the tr/trf/trn calls in src: missing, unused and mismatched {n}
# a catalog line is "english<TAB>translation", or for trn "one<TAB>other<TAB>singular<TAB>plural"
# usage: check_translations.py [--missing LANG] (prints the missing keys of LANG as catalog lines)
import pathlib
import re
import sys

root = pathlib.Path(__file__).resolve().parent.parent
call = re.compile(r'\btr([fn]?)\(')
literal = re.compile(r'L"((?:[^"\\]|\\.)*)"')
placeholder = re.compile(r'\{\d\}')


def c_unescape(text):
    out, i = [], 0
    while i < len(text):
        if text[i] == '\\' and i + 1 < len(text):
            out.append({'t': '\t', 'n': '\n', 'r': '\r', '\\': '\\', '"': '"', "'": "'"}.get(text[i + 1], text[i + 1]))
            i += 2
        else:
            out.append(text[i])
            i += 1
    return ''.join(out)


def top_level_args(source, start):
    # start is just after "("; returns the arguments' text up to the matching ")"
    args, depth, i, begin = [], 0, start, start
    while i < len(source):
        c = source[i]
        if c == '"':
            i += 1
            while source[i] != '"':
                i += 2 if source[i] == '\\' else 1
        elif c in '([{':
            depth += 1
        elif c in ')]}':
            if depth == 0:
                args.append(source[begin:i])
                return args
            depth -= 1
        elif c == ',' and depth == 0:
            args.append(source[begin:i])
            begin = i + 1
        i += 1
    return args


def keys_in(expression):
    # adjacent literals are one string; a ternary gives one key per branch
    keys, pieces, last_end = [], [], None
    for m in literal.finditer(expression):
        if pieces and expression[last_end:m.start()].strip() == '':
            pieces.append(m.group(1))
        else:
            if pieces:
                keys.append(''.join(pieces))
            pieces = [m.group(1)]
        last_end = m.end()
    if pieces:
        keys.append(''.join(pieces))
    return [c_unescape(k) for k in keys]


def source_keys():
    found = {}
    for path in sorted((root / 'src').glob('*.cpp')):
        if path.name == 'i18n.cpp':
            continue
        text = path.read_text(encoding='utf-8')
        for m in call.finditer(text):
            args = top_level_args(text, m.end())
            line = text.count('\n', 0, m.start()) + 1
            if m.group(1) == 'n':
                one, other = keys_in(args[0]), keys_in(args[1])
                if len(one) != 1 or len(other) != 1:
                    sys.exit(f'{path.name}:{line}: trn needs one literal for each form')
                found.setdefault((one[0], other[0]), f'{path.name}:{line}')
            else:
                for key in keys_in(args[0]):
                    found.setdefault(key, f'{path.name}:{line}')
    return found


def catalog_escape(text):
    return text.replace('\\', '\\\\').replace('\t', '\\t').replace('\n', '\\n')


def catalog_unescape(text):
    return c_unescape(text)


def read_catalog(path):
    entries, problems = {}, []
    for number, line in enumerate(path.read_text(encoding='utf-8').splitlines(), 1):
        if not line or line.startswith('#'):
            continue
        if '\t' not in line:
            problems.append(f'{path.name}:{number}: no tab')
            continue
        columns = [catalog_unescape(c) for c in line.split('\t')]
        if len(columns) == 2:
            english, translated = columns[0], [columns[1]]
        elif len(columns) == 4:
            english, translated = (columns[0], columns[1]), columns[2:]
        else:
            problems.append(f'{path.name}:{number}: {len(columns)} columns')
            continue
        if english in entries:
            problems.append(f'{path.name}:{number}: duplicate {english!r}')
        if any(not t.strip() for t in translated):
            problems.append(f'{path.name}:{number}: empty translation for {english!r}')
        entries[english] = translated
    return entries, problems


def main():
    keys = source_keys()
    catalogs = sorted((root / 'res' / 'lang').glob('*.tsv'))
    if len(sys.argv) == 3 and sys.argv[1] == '--missing':
        entries, _ = read_catalog(root / 'res' / 'lang' / f'{sys.argv[2]}.tsv')
        for key in keys:
            if key not in entries:
                english = key if isinstance(key, tuple) else (key,)
                print('\t'.join(catalog_escape(k) for k in english) + '\t' * len(english))
        return 0
    failed = False
    for path in catalogs:
        entries, problems = read_catalog(path)
        for key, where in keys.items():
            if key not in entries:
                problems.append(f'{path.name}: missing {key!r} ({where})')
                continue
            english = key if isinstance(key, tuple) else (key,)
            for source, translated in zip(english, entries[key]):
                if sorted(set(placeholder.findall(source))) != sorted(set(placeholder.findall(translated))):
                    problems.append(f'{path.name}: placeholders differ for {source!r}')
                elif source.count('\t') != translated.count('\t'):
                    problems.append(f'{path.name}: tabs differ for {source!r}')
        for key in entries:
            if key not in keys:
                problems.append(f'{path.name}: unused {key!r}')
        for problem in problems:
            print(problem)
        print(f'{path.name}: {len(entries)} entries, {len(keys)} keys in the sources, {len(problems)} problems')
        failed = failed or bool(problems)
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
