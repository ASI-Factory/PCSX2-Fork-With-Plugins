"""Update marked fork release sections while preserving manually written notes."""
import argparse
import os
from pathlib import Path
import re


def compose(body, snippets):
    for snippet in snippets:
        snippet = snippet.strip()
        marker = re.match(r'<!-- ([a-z0-9-]+):begin -->', snippet)
        if not marker:
            raise ValueError('Release section needs an opening marker')
        begin, end = marker[0], f'<!-- {marker[1]}:end -->'
        if snippet.count(begin) != 1 or snippet.count(end) != 1 or not snippet.endswith(end):
            raise ValueError('Ambiguous release section')
        if body.count(begin) != body.count(end) or body.count(begin) > 1:
            raise ValueError('Ambiguous existing release section')
        if begin in body:
            before, rest = body.split(begin, 1)
            if end not in rest:
                raise ValueError('Release markers are out of order')
            _, after = rest.split(end, 1)
            body = before + snippet + after
        else:
            body = body.rstrip() + '\n\n' + snippet + '\n'
    return body


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('snippets', type=Path, nargs='+')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    result = compose(os.environ.get('EXISTING_RELEASE_BODY', ''),
                     [path.read_text(encoding='utf-8') for path in args.snippets])
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(result, encoding='utf-8')


if __name__ == '__main__':
    main()
