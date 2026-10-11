"""Collect cache residency evidence; raw daemon logs stay on the disposable runner."""
import argparse
import json
from pathlib import Path
import re


def entries(directory):
    return {p.name: p.stat().st_size for p in Path(directory).glob('*/*/*')
            if p.is_file() and re.fullmatch(r'[0-9a-f]{64}', p.name)}


def report(before, log, after):
    records = []
    hits = 0
    misses = 0
    for line in log.splitlines():
        match = re.search(r'\[([^\]]+)\]: Hash key: ([0-9a-f]{64})', line)
        if match:
            obj, key = match.groups()
            records.append({'object': obj, 'key': key, 'resident_at_restore': key in before})
        match = re.search(r'\[([^\]]+)\]: Cache (hit|miss) in', line)
        if match:
            hits += match[2] == 'hit'
            misses += match[2] == 'miss'
    return {'restored_entries': len(before), 'restored_bytes': sum(before.values()),
            'final_entries': len(after), 'final_bytes': sum(after.values()),
            'requests': len(records),
            'completed_lookups': hits + misses,
            'hits': hits, 'misses': misses,
            'unresolved': len(records) - hits - misses,
            'resident_at_restore': sum(r['resident_at_restore'] for r in records),
            'records': records}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('mode', choices=['snapshot', 'report'])
    parser.add_argument('--cache', required=True)
    parser.add_argument('--output', required=True)
    parser.add_argument('--before')
    parser.add_argument('--log')
    parser.add_argument('--stats')
    args = parser.parse_args()
    result = entries(args.cache)
    if args.mode == 'report':
        if not args.before or not args.log or not args.stats:
            parser.error('report requires --before, --log and --stats')
        result = report(json.loads(Path(args.before).read_text(encoding='utf-8')),
                        Path(args.log).read_text(encoding='utf-8', errors='replace'), result)
        if not result['requests']:
            raise SystemExit('No compiler cache keys captured; probe is unavailable')
        result['statistics'] = json.loads(Path(args.stats).read_text(encoding='utf-8-sig'))
        result['complete'] = (result['requests'] == result['completed_lookups'] ==
                              result['statistics']['stats']['requests_executed'])
        print(json.dumps({k: v for k, v in result.items() if k != 'records'}, indent=2))
    Path(args.output).write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')


if __name__ == '__main__':
    main()
