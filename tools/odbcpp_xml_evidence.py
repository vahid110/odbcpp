"""Bounded, DTD-free XML evidence parsing; no admission or result assertions.

Files are read incrementally, including one overflow byte, never with read_bytes.
Expat handles encoding/namespace syntax; declaration callbacks reject DTDs and
entities even in UTF-16 or when never referenced. No external entity resolver is
created. Limits apply before Element allocation; unfinished lexical tokens and
text are bounded independently of the whole input. Keep Expat's default reparse
deferral enabled on interpreters that support it.
"""
from dataclasses import dataclass
from pathlib import Path
from xml.parsers import expat
import xml.etree.ElementTree as ET


@dataclass(frozen=True)
class XmlLimits:
    max_bytes: int = 64 * 1024 * 1024
    max_depth: int = 128
    max_nodes: int = 250000
    max_token_bytes: int = 8 * 1024 * 1024
    max_markup_bytes: int = 64 * 1024
    max_attributes: int = 256

    def __post_init__(self):
        for value in vars(self).values():
            if type(value) is not int or value < 1:
                raise ValueError('XML limits must be positive integers')


class XmlSafetyError(ET.ParseError):
    """A disallowed declaration or resource budget, caught as a parse failure."""


class XmlDeclarationError(XmlSafetyError):
    """A declaration/markup restriction, distinct from resource budgets."""


DEFAULT_LIMITS = XmlLimits()
CHUNK_BYTES = 64 * 1024


def _expanded_name(name):
    return '{' + name if '}' in name else name


class _EvidenceParser:
    def __init__(self, limits, forbid_misc=False):
        if type(forbid_misc) is not bool:
            raise ValueError('XML markup policy must be boolean')
        self.limits = limits
        self.builder = ET.TreeBuilder()
        self.parser = expat.ParserCreate(namespace_separator='}')
        self.parser.StartElementHandler = self.start
        self.parser.EndElementHandler = self.end
        self.parser.CharacterDataHandler = self.text
        self.parser.StartDoctypeDeclHandler = self.forbid_declaration
        self.parser.EntityDeclHandler = self.forbid_declaration
        self.parser.UnparsedEntityDeclHandler = self.forbid_declaration
        self.parser.ExternalEntityRefHandler = self.forbid_declaration
        self.parser.SetParamEntityParsing(expat.XML_PARAM_ENTITY_PARSING_NEVER)
        self.parser.CommentHandler = self.forbid_declaration if forbid_misc else self.ignored_token
        self.parser.ProcessingInstructionHandler = self.forbid_declaration if forbid_misc else self.ignored_token
        if forbid_misc:
            self.parser.StartCdataSectionHandler = self.forbid_declaration
        self.parser.StartNamespaceDeclHandler = self.ignored_token
        self.depth = 0
        self.nodes = 0
        self.text_bytes = 0
        self.fed_bytes = 0

    def forbid_declaration(self, *_args):
        raise XmlDeclarationError('Disallowed declaration or markup in XML evidence')

    def token(self, *parts):
        size = sum(len(part.encode('utf-8')) for part in parts if part is not None)
        if size > min(self.limits.max_token_bytes, self.limits.max_markup_bytes):
            raise XmlSafetyError('XML evidence token exceeds byte limit')

    def ignored_token(self, *parts):
        self.token(*parts)

    def start(self, name, attributes):
        # Do not allocate an Element beyond the depth/node budget.
        if self.depth >= self.limits.max_depth or self.nodes >= self.limits.max_nodes:
            raise XmlSafetyError('XML evidence exceeds depth or node limit')
        if len(attributes) > self.limits.max_attributes:
            raise XmlSafetyError('XML evidence exceeds attribute limit')
        self.token(name, *attributes.keys(), *attributes.values())
        self.depth += 1
        self.nodes += 1
        self.text_bytes = 0
        self.builder.start(_expanded_name(name),
                           {_expanded_name(key): value for key, value in attributes.items()})

    def end(self, name):
        self.token(name)
        self.builder.end(_expanded_name(name))
        self.depth -= 1
        self.text_bytes = 0

    def text(self, data):
        self.text_bytes += len(data.encode('utf-8'))
        if self.text_bytes > self.limits.max_token_bytes:
            raise XmlSafetyError('XML evidence text exceeds token limit')
        self.builder.data(data)

    def feed(self, data, final=False):
        self.fed_bytes += len(data)
        if self.fed_bytes > self.limits.max_bytes:
            raise XmlSafetyError('XML evidence exceeds byte limit')
        try:
            self.parser.Parse(data, final)
        except expat.ExpatError as error:
            converted = ET.ParseError(str(error))
            converted.code = error.code
            converted.position = (error.lineno, error.offset)
            raise converted from None
        # Expat may defer callbacks for incomplete attributes/comments/etc.
        # Reject the buffered unfinished token without waiting for its terminator.
        if self.fed_bytes - self.parser.CurrentByteIndex > min(
                self.limits.max_token_bytes, self.limits.max_markup_bytes):
            raise XmlSafetyError('XML evidence unfinished token exceeds byte limit')

    def close(self):
        self.feed(b'', final=True)
        return self.builder.close()


def parse_xml_file(path, *, limits=DEFAULT_LIMITS, forbid_misc=False):
    """Return an Element from capped binary reads, leaving raw evidence intact."""
    parser = _EvidenceParser(limits, forbid_misc)
    with Path(path).open('rb') as stream:
        while True:
            # The one-byte sentinel proves overflow without a whole-file read.
            size = min(CHUNK_BYTES, limits.max_bytes - parser.fed_bytes + 1)
            block = stream.read(size)
            if not block:
                break
            parser.feed(block)
    return parser.close()


def parse_xml_bytes(data, *, limits=DEFAULT_LIMITS, forbid_misc=False):
    """Parse an already bounded byte buffer, preserving its declared encoding."""
    if not isinstance(data, bytes):
        raise TypeError('XML evidence must be bytes, not predecoded text')
    if len(data) > limits.max_bytes:
        raise XmlSafetyError('XML evidence exceeds byte limit')
    parser = _EvidenceParser(limits, forbid_misc)
    for offset in range(0, len(data), CHUNK_BYTES):
        parser.feed(data[offset:offset + CHUNK_BYTES])
    return parser.close()
