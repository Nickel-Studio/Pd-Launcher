import argparse
import pathlib
import shutil
import subprocess
import tempfile


def run_steps(executable, root, faults):
    subprocess.run([str(executable), '--steps', str(root)], check=True, timeout=90)
    if faults:
        phases = ('step.first', 'files.prepared', 'files.planned', 'files.backed_up',
                  'files.published', 'files.retained', 'step.marker', 'step.committed',
                  'step.nonfinal', 'cleanup', 'rollback.next', 'rollback.previous')
        for mode in ('same', 'renamed', 'discard'):
            for phase in phases:
                if mode == 'discard' and phase in ('files.retained', 'rollback.next'):
                    continue
                case = root / f'step-{mode}-{phase}'
                case.mkdir()
                subprocess.run([str(executable), '--crash-step', str(case), phase, mode], check=True, timeout=15)
                subprocess.run([str(executable), '--recover-step', str(case), phase, mode], check=True, timeout=15)
                shutil.rmtree(case)
        for kind in ('prefix-content', 'prefix-identity', 'suffix-content', 'retained-content', 'duplicate-id', 'missing-step'):
            case = root / f'step-conflict-{kind}'
            case.mkdir()
            subprocess.run([str(executable), '--crash-step', str(case), 'files.retained', 'renamed'], check=True, timeout=15)
            if kind in ('duplicate-id', 'missing-step'):
                import json
                journal_path = next(case.glob('.nickel-task-*/journal.json'))
                journal = json.loads(journal_path.read_text(encoding='utf-8'))
                if kind == 'duplicate-id':
                    journal['files'][1]['stepId'] = journal['files'][0]['stepId']
                else:
                    del journal['files'][1]['stepId']
                journal_path.write_text(json.dumps(journal), encoding='utf-8')
            else:
                victim = case / 'mods' / ('prefix/keep.jar' if kind.startswith('prefix-') else
                                          'retained.jar' if kind.startswith('retained-') else 'updated.jar')
                if kind == 'prefix-identity':
                    data = victim.read_bytes()
                    victim.unlink()
                    victim.write_bytes(data)
                else:
                    victim.write_bytes(b'external data')
            subprocess.run([str(executable), '--recovery-conflict', str(case)], check=True, timeout=15)
            shutil.rmtree(case)
        print('PASS file steps 34 real crash/recovery windows and 6 fail-closed content/identity/schema conflicts')


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--executable', required=True)
    parser.add_argument('--workspace', required=True)
    parser.add_argument('--faults', action='store_true')
    parser.add_argument('--steps', action='store_true')
    args = parser.parse_args()
    executable = pathlib.Path(args.executable).resolve()
    parent = pathlib.Path(args.workspace).resolve()
    parent.mkdir(parents=True, exist_ok=True)
    root = pathlib.Path(tempfile.mkdtemp(prefix='task-process-', dir=parent))
    passed = False
    try:
        if args.steps:
            run_steps(executable, root, args.faults)
        else:
            subprocess.run([str(executable), str(root)], check=True, timeout=90)
            subprocess.run([str(executable), '--crash', str(root)], check=True, timeout=15)
            subprocess.run([str(executable), '--recover', str(root)], check=True, timeout=15)
            if list(root.iterdir()):
                raise RuntimeError('owned crash fixture contains unrecovered files')
            if args.faults:
                for mode in ('same', 'renamed'):
                    for phase in ('files.planned', 'files.backed_up', 'files.published', 'files.retained', 'files.committed', 'rollback.next', 'rollback.previous'):
                        case = root / f'{mode}-{phase}'
                        case.mkdir()
                        subprocess.run([str(executable), '--crash-commit', str(case), phase, mode], check=True, timeout=15)
                        state = 'committed' if phase == 'files.committed' else 'uncommitted'
                        subprocess.run([str(executable), '--recover-commit', str(case), state, mode], check=True, timeout=15)
                        shutil.rmtree(case)
                for kind in ('content', 'identity', 'original', 'backup'):
                    case = root / f'conflict-{kind}'
                    case.mkdir()
                    subprocess.run([str(executable), '--crash-commit', str(case), 'files.published', 'renamed'], check=True, timeout=15)
                    victim = next(case.glob('.nickel-task-*/backup/0')) if kind == 'backup' else case / ('original.jar' if kind == 'original' else 'updated.jar')
                    if kind == 'identity':
                        contents = victim.read_bytes()
                        victim.unlink()
                        victim.write_bytes(contents)
                    else:
                        victim.write_bytes(b'external data')
                    subprocess.run([str(executable), '--recovery-conflict', str(case)], check=True, timeout=15)
                    shutil.rmtree(case)
                print('PASS actual same/renamed commits interrupted before journal updates, rollback interruption, and external conflict retention')
        deep = root / 'long-path'
        while len(str(deep)) < 420:
            deep /= 'segment-' + 'x' * 48
        deep.mkdir(parents=True)
        if args.steps:
            subprocess.run([str(executable), '--steps', str(deep)], check=True, timeout=90)
            if args.faults:
                case = deep / 'deep-step'
                case.mkdir()
                subprocess.run([str(executable), '--crash-step', str(case), 'files.retained', 'renamed'], check=True, timeout=15)
                subprocess.run([str(executable), '--recover-step', str(case), 'files.retained', 'renamed'], check=True, timeout=15)
        else:
            subprocess.run([str(executable), str(deep)], check=True, timeout=90)
            subprocess.run([str(executable), '--crash', str(deep)], check=True, timeout=15)
            subprocess.run([str(executable), '--recover', str(deep)], check=True, timeout=15)
            if args.faults:
                case = deep / 'deep-replacement'
                case.mkdir()
                subprocess.run([str(executable), '--crash-commit', str(case), 'files.retained', 'renamed'], check=True, timeout=15)
                subprocess.run([str(executable), '--recover-commit', str(case), 'uncommitted', 'renamed'], check=True, timeout=15)
        print(f'PASS task long logical path characters={len(str(deep))}')
        print('PASS task process interruption and clean recovery')
        passed = True
    finally:
        if passed:
            shutil.rmtree(root)
        else:
            print(f'FAILED task evidence retained: {root}')


if __name__ == '__main__':
    main()
