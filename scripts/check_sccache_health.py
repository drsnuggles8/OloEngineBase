"""Fail visibly when a complete hosted build cannot reuse its restored compiler cache."""
import argparse
import json
import os
from pathlib import Path

from sccache_object_manifest import compare


def counter(value):
    if type(value) is not int or not 0 <= value <= 2**64 - 1:
        raise ValueError('expected an unsigned 64-bit integer counter')
    return value


def assess(data, restored_key, build_outcome, prediction=None, minimum_requests=500,
           minimum_hit_rate=5.0, minimum_predicted=100, minimum_reuse_percent=50.0):
    """Classify a build's compiler reuse.

    `prediction` is sccache_object_manifest.compare(producer, current): how many objects kept
    exactly the inputs the restored snapshot was saved with. With it, hits are judged against
    the objects the cache must serve, so a change that legitimately invalidates most of the
    tree (a widely included header, a compile flag) is not a broken cache. Without it -- a
    snapshot saved before the manifest existed, or one whose manifest was lost -- the
    raw-rate floor applies and the report says so.
    """
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
    elif prediction is None:
        if result['hit_rate_percent'] < minimum_hit_rate:
            result.update(disposition='near-zero-warm',
                          reason=f'warm restore returned less than {minimum_hit_rate:g}% hits '
                                 '(the snapshot carries no object manifest, so judged on the raw rate)')
        else:
            result.update(disposition='healthy-warm',
                          reason='restored cache demonstrated compiler reuse '
                                 '(the snapshot carries no object manifest)')
    else:
        predicted = prediction['predicted_reusable']
        result['prediction'] = prediction
        changed = (f"{prediction['changed_files']} input files changed since the producer, e.g. "
                   f"{', '.join(prediction['changed_file_sample'][:3]) or 'none'}")
        if predicted < minimum_predicted:
            result.update(disposition='inputs-changed',
                          reason=f'only {predicted} objects kept their inputs since the producer '
                                 f'(fewer than {minimum_predicted}), so there is no reuse to measure; '
                                 + changed)
        else:
            result['reuse_of_predicted_percent'] = 100.0 * result['hits'] / predicted
            if result['reuse_of_predicted_percent'] < minimum_reuse_percent:
                result.update(disposition='reuse-shortfall',
                              reason=f"{result['hits']} hits for {predicted} objects whose inputs are "
                                     f'unchanged since the producer, below {minimum_reuse_percent:g}%: '
                                     'the restored cache is not serving what it holds; ' + changed)
            else:
                result.update(disposition='healthy-warm',
                              reason=f"{result['hits']} hits for {predicted} objects whose inputs are "
                                     'unchanged since the producer')
    return result


def load_prediction(restored_path, current_path):
    """None when the snapshot has no manifest (saved before manifests existed, or by a probe).

    A restored manifest without a current one is not that case: this build failed to record
    its own, and judging it on the raw rate would hide the broken step, so it raises.
    """
    if not restored_path or not Path(restored_path).is_file():
        return None
    if not current_path or not Path(current_path).is_file():
        raise ValueError('the snapshot carries an object manifest but this build recorded none')
    restored = json.loads(Path(restored_path).read_text(encoding='utf-8'))
    current = json.loads(Path(current_path).read_text(encoding='utf-8'))
    return compare(restored, current)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--stats', required=True)
    parser.add_argument('--output', required=True)
    parser.add_argument('--restored-manifest', help='object manifest the restored snapshot was saved with')
    parser.add_argument('--current-manifest', help="this build's object manifest")
    args = parser.parse_args()
    restored_key = os.environ.get('OLO_CACHE_RESTORED_KEY', '')
    build_outcome = os.environ.get('OLO_CACHE_BUILD_OUTCOME', '')
    try:
        data = json.loads(Path(args.stats).read_text(encoding='utf-8-sig'))
        prediction = load_prediction(args.restored_manifest, args.current_manifest) if restored_key else None
        result = assess(data, restored_key, build_outcome, prediction)
    except (OSError, ValueError, KeyError, TypeError) as error:
        result = {'disposition': 'unavailable', 'reason': f'statistics or manifests unavailable: {error}',
                  'restored_key': restored_key, 'build_outcome': build_outcome}
    Path(args.output).write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    healthy = result['disposition'] in ('cold', 'inputs-changed', 'healthy-warm')
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
