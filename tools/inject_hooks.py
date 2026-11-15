import collections
import os
import shutil
import sys


def load_hooks(path):
    by_file = collections.OrderedDict()
    with open(path, encoding='utf-8-sig') as fh:
        for number, line in enumerate(fh, 1):
            line = line.rstrip('\r\n')
            if not line:
                continue
            parts = line.split('\t', 2)
            if len(parts) != 3:
                sys.exit('hooks file line %d: expected file, label and code separated by tabs' % number)
            name, label, code = parts
            if code.count('{') != code.count('}'):
                sys.exit('hooks file line %d: unbalanced braces in the hook for %s' % (number, label))
            by_file.setdefault(name, collections.OrderedDict()).setdefault(label, []).append(code)
    return by_file


def inject(hooks_path, source_dir, output_dir):
    by_file = load_hooks(hooks_path)
    if not os.path.isdir(source_dir):
        sys.exit('generated source folder not found: ' + source_dir)
    if os.path.abspath(source_dir) == os.path.abspath(output_dir):
        sys.exit('the output folder must differ from the generated source folder')
    os.makedirs(output_dir, exist_ok=True)
    for name in os.listdir(source_dir):
        if name not in by_file:
            shutil.copyfile(os.path.join(source_dir, name), os.path.join(output_dir, name))
    total = sum(len(codes) for labels in by_file.values() for codes in labels.values())
    done = 0
    missing = []
    for name, labels in by_file.items():
        path = os.path.join(source_dir, name)
        if not os.path.isfile(path):
            missing.extend((name, label) for label in labels)
            continue
        out = []
        seen = set()
        with open(path, encoding='utf-8', newline='') as fh:
            for line in fh:
                out.append(line)
                text = line.strip()
                if text.startswith('loc_') and text.endswith(': ;'):
                    label = text[:-3]
                    if label in labels and label not in seen:
                        seen.add(label)
                        newline = '\r\n' if line.endswith('\r\n') else '\n'
                        for code in labels[label]:
                            out.append('    ' + code + newline)
                            done += 1
        missing.extend((name, label) for label in labels if label not in seen)
        with open(os.path.join(output_dir, name), 'w', encoding='utf-8', newline='') as fh:
            fh.write(''.join(out))
    print('%d hooks, %d injected, %d missing' % (total, done, len(missing)))
    for name, label in missing:
        print('MISSING', name, label)
    return 1 if missing else 0


def main(argv):
    if len(argv) != 4:
        sys.exit('usage: inject_hooks.py HOOKS_FILE GENERATED_SOURCE_DIR OUTPUT_DIR')
    return inject(argv[1], argv[2], argv[3])


if __name__ == '__main__':
    sys.exit(main(sys.argv))
