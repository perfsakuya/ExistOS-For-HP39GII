import re
import sys


with open(sys.argv[1], 'rt', encoding='ascii') as source, open(sys.argv[2], 'wb') as output:
    header = source.readline()
    match = re.fullmatch(r'const unsigned char doom_iwad\[(\d+)UL\] = \{\n', header)
    assert match, header
    expected = int(match.group(1))
    count = 0
    for line in source:
        if line.startswith('};'):
            break
        values = re.findall(r'0x([0-9a-fA-F]{2})', line)
        output.write(bytes(int(value, 16) for value in values))
        count += len(values)
    assert count == expected, (count, expected)
print('WAD bytes', count)
