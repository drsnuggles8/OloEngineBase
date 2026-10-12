"""Fail visibly when a complete hosted build cannot reuse its restored compiler cache."""
import argparse
import json
import os
from pathlib import Path


def counter(value):
    if type(value) is not int or not 0 <= value <= 2**64 - 1:
        raise ValueError('expected an unsigned 64-bit integer counter')
    return value


def assess(data, restored_key, build_outcome, minimum_requests=500, minimum_hit_rate=5.0):
    result = {'restored_key': restored_key, 'build_outcome': build_outcome,
              'disposition': 'unavailable'}
    try:
        stats = data['stats']
        requests = counter(stats['compile_requests'])
        executed = counter(stats['requests_executed'])
        hits = sum(counter(v) for v in stats['cache_hits']['counts'].values())
        misses = sum(counter(v) for v in stats['cache_misses']['counts'].values())
        for name in ('cache_read_errors', 'cache_write_errors', 'compile_fails'):
            if counter(stats[name]):
                raise ValueError(f'{name} is nonzero')
        if requests < executed or hits + misses != executed:
            raise ValueError('compile and cache lookup totals disagree')
        result.update(requests=requests, executed=executed, hits=hits, misses=misses,
                      hit_rate_percent=100.0 * hits / executed if executed else 0.0,
                      cache_size_bytes=data.get('cache_size'), max_cache_size_bytes=data.get('max_cache_size'))
    except (KeyError, TypeError, AttributeError, ValueError) as error:
        result['reason'] = f'invalid sccache statistics: {error}'
        return result
    if build_outcome != 'success':
        result.update(disposition='incomplete', reason=f'build outcome is {build_outcome or "missing"}')
    elif executed < minimum_requests:
        result.update(disposition='insufficient-sample',
                      reason=f'only {executed} cacheable requests; expected at least {minimum_requests}')
    elif not restored_key:
        result.update(disposition='cold', reason='no Actions cache snapshot was restored')
    elif result['hit_rate_percent'] < minimum_hit_rate:
        result.update(disposition='near-zero-warm',
                      reason=f'warm restore returned less than {minimum_hit_rate:g}% hits')
    else:
        result.update(disposition='healthy-warm', reason='restored cache demonstrated compiler reuse')
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--stats', required=True)
    parser.add_argument('--output', required=True)
    args = parser.parse_args()
    restored_key = os.environ.get('OLO_CACHE_RESTORED_KEY', '')
    build_outcome = os.environ.get('OLO_CACHE_BUILD_OUTCOME', '')
    try:
        data = json.loads(Path(args.stats).read_text(encoding='utf-8-sig'))
        result = assess(data, restored_key, build_outcome)
    except (OSError, ValueError) as error:
        result = {'disposition': 'unavailable', 'reason': f'statistics unavailable: {error}',
                  'restored_key': restored_key, 'build_outcome': build_outcome}
    Path(args.output).write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    healthy = result['disposition'] in ('cold', 'healthy-warm')
    counts = (f"{result['hits']}/{result['executed']} hits ({result['hit_rate_percent']:.2f}%)"
              if 'hits' in result else 'counts unavailable')
    message = f"sccache {result['disposition']}: {counts}; {result['reason']}"
    print(('::notice::' if healthy else '::error::') + message)
    summary = os.environ.get('GITHUB_STEP_SUMMARY')
    if summary:
        with Path(summary).open('a', encoding='utf-8') as stream:
            stream.write(f"\n### Windows compiler cache\n\n{message}\n\n"
                         f"Restored key: `{restored_key or 'none'}`\n\n"
                         f"Cache bytes: {result.get('cache_size_bytes', 'unknown')}; "
                         f"cap bytes: {result.get('max_cache_size_bytes', 'unknown')}\n")
    return 0 if healthy else 1


if __name__ == '__main__':
    raise SystemExit(main())
