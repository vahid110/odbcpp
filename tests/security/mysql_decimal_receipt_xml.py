"""Bounded receipt XML inventory validation; no native qualification or I/O policy."""
import hashlib
import os
from pathlib import Path
import re
import stat
from xml.parsers import expat

SUITE = 'MySqlDecimalReceiptObservationIntegrationTest'
CASE = 'ActualCastParameterMetadataRefusalIsObserved'
MAX_XML_BYTES = 16384
ERROR = 'Unexpected MySQL DECIMAL receipt observation inventory'
EXACT = {
    'decimal_receipt_schema': 1, 'prepare_parameter_count': 1,
    'prepare_result_count': 2, 'observed_parameter_count': 1,
    'records_truncated': 0, 'exchange_complete': 0,
    'transport_complete_calls': 1, 'prepare_command_count': 1,
    'execute_command_count': 0, 'close_command_count': 0,
    'raw_parameter_0_native_type': 246, 'runtime_retired': 1,
    'physical_close_delta': 1, 'parameter_support_claimed': 0,
}
RANGES = {
    'raw_parameter_0_charset': 65535,
    'raw_parameter_0_byte_width': 4294967295,
    'raw_parameter_0_decimals': 255,
    'raw_parameter_0_flags': 65535,
    'raw_parameter_0_unsigned': 1,
    'refusal_policy_code': 3,
}


def _reject(*_args):
    raise RuntimeError(ERROR)


def _uint(value, maximum):
    if (not isinstance(value, str) or len(value) > len(str(maximum))
            or re.fullmatch(r'0|[1-9][0-9]*', value, flags=re.ASCII) is None):
        _reject()
    parsed = int(value)
    if parsed > maximum:
        _reject()
    return parsed


def _attributes(node, required, optional=()):
    attributes = node['attributes']
    if (not set(required).issubset(attributes)
            or not set(attributes).issubset(set(required) | set(optional))):
        _reject()
    for key, value in required.items():
        if attributes[key] != value:
            _reject()
    for key in optional:
        if key not in attributes:
            continue
        value = attributes[key]
        if key == 'name' and value != 'AllTests':
            _reject()
        if key == 'skipped' and value != '0':
            _reject()
        if key == 'line' and _uint(value, 4294967295) == 0:
            _reject()
        if key == 'time' and re.fullmatch(r'(?:0|[1-9][0-9]*)\.[0-9]*', value, flags=re.ASCII) is None:
            _reject()
        if key == 'timestamp' and re.fullmatch(r'[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}(?:\.[0-9]+)?', value, flags=re.ASCII) is None:
            _reject()
        if len(value.encode('utf-8')) > (1024 if key == 'file' else 128):
            _reject()


def _parse(raw):
    if not isinstance(raw, bytes) or not raw or len(raw) > MAX_XML_BYTES:
        _reject()
    text = raw.decode('utf-8-sig', errors='strict')
    if '\0' in text:
        _reject()
    # This narrow emitter format needs only ordinary predefined escapes.
    # Numeric references cannot smuggle nonliteral canonical property digits.
    for reference in re.findall(r'&([^;]*);', text):
        if reference not in ('amp', 'lt', 'gt', 'quot', 'apos'):
            _reject()
    parser = expat.ParserCreate(encoding='UTF-8')
    stack = []
    roots = []
    nodes = 0

    def start(name, attributes):
        nonlocal nodes
        nodes += 1
        if nodes > 24 or len(stack) >= 5 or name not in ('testsuites', 'testsuite', 'testcase', 'properties', 'property'):
            _reject()
        if len(attributes) > 8:
            _reject()
        if any(':' in key or key == 'xmlns' or len(value.encode('utf-8')) > 1024 for key, value in attributes.items()):
            _reject()
        node = {'tag': name, 'attributes': dict(attributes), 'children': []}
        if stack:
            stack[-1]['children'].append(node)
        else:
            roots.append(node)
        stack.append(node)

    def characters(data):
        if (stack and stack[-1]['tag'] == 'property' and data) or data.strip(' \t\r\n'):
            _reject()

    def declaration(version, encoding, standalone):
        if version != '1.0' or (encoding is not None and encoding.upper() != 'UTF-8'):
            _reject()

    parser.StartElementHandler = start
    parser.EndElementHandler = lambda _name: stack.pop()
    parser.CharacterDataHandler = characters
    parser.XmlDeclHandler = declaration
    parser.StartDoctypeDeclHandler = _reject
    parser.EntityDeclHandler = _reject
    parser.ExternalEntityRefHandler = _reject
    parser.CommentHandler = _reject
    parser.ProcessingInstructionHandler = _reject
    parser.StartCdataSectionHandler = _reject
    parser.Parse(text, True)
    if len(roots) != 1 or nodes != 24:
        _reject()
    root = roots[0]
    if root['tag'] != 'testsuites' or len(root['children']) != 1:
        _reject()
    _attributes(root, {'tests': '1', 'failures': '0', 'errors': '0', 'disabled': '0'},
                ('skipped', 'name', 'time', 'timestamp'))
    suite = root['children'][0]
    if suite['tag'] != 'testsuite' or len(suite['children']) != 1:
        _reject()
    _attributes(suite, {'name': SUITE, 'tests': '1', 'failures': '0', 'errors': '0', 'disabled': '0', 'skipped': '0'},
                ('time', 'timestamp'))
    case = suite['children'][0]
    if case['tag'] != 'testcase' or len(case['children']) != 1:
        _reject()
    _attributes(case, {'name': CASE, 'classname': SUITE, 'status': 'run', 'result': 'completed'},
                ('file', 'line', 'time', 'timestamp'))
    properties = case['children'][0]
    if properties['tag'] != 'properties' or properties['attributes'] or len(properties['children']) != 20:
        _reject()
    values = {}
    for node in properties['children']:
        if node['tag'] != 'property' or node['children'] or set(node['attributes']) != {'name', 'value'}:
            _reject()
        key = node['attributes']['name']
        if key in values or key not in EXACT.keys() | RANGES.keys():
            _reject()
        maximum = RANGES.get(key, EXACT.get(key))
        values[key] = _uint(node['attributes']['value'], maximum)
    if set(values) != EXACT.keys() | RANGES.keys() or any(values[key] != value for key, value in EXACT.items()):
        _reject()
    precision = values['raw_parameter_0_decimals']
    width = values['raw_parameter_0_byte_width']
    flags = values['raw_parameter_0_flags']
    unsigned = bool(flags & 32)
    if values['raw_parameter_0_unsigned'] != int(unsigned):
        _reject()
    overhead = (1 if precision else 0) + (0 if unsigned else 1)
    if width <= overhead or precision > width - overhead:
        policy = 1
    elif width - overhead > 65 or precision > 30:
        policy = 2
    else:
        policy = 3
    if values['refusal_policy_code'] != policy:
        _reject()
    # Only owning scalar evidence; no Element, file path or stdout authority.
    return {
        'schemaVersion': 1, 'suite': SUITE, 'case': CASE,
        'properties': dict(values),
        'rawParameter': {'nativeType': 246, 'charset': values['raw_parameter_0_charset'],
                         'byteWidth': width, 'decimals': precision,
                         'flags': flags, 'unsignedValue': unsigned},
        'exchangeComplete': False, 'parameterSupportClaimed': False,
        'refusalPolicyCode': policy, 'xmlSha256': hashlib.sha256(raw).hexdigest(),
    }


def validate_decimal_receipt_xml_bytes(raw: bytes) -> dict:
    """Validate bounded bytes only; success describes inventory, not native proof."""
    try:
        return _parse(raw)
    except Exception:
        raise RuntimeError(ERROR) from None


def validate_decimal_receipt_xml(path: Path) -> dict:
    """Read at most 16KiB+1 once; hash and validate the exact same owned bytes."""
    try:
        flags = os.O_RDONLY | getattr(os, 'O_NONBLOCK', 0)
        with os.fdopen(os.open(path, flags), 'rb') as stream:
            if not stat.S_ISREG(os.fstat(stream.fileno()).st_mode):
                _reject()
            raw = stream.read(MAX_XML_BYTES + 1)
        return validate_decimal_receipt_xml_bytes(raw)
    except Exception:
        raise RuntimeError(ERROR) from None
