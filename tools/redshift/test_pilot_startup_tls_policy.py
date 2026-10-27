"""Exercise the startup fixture's actual context setup without operational imports.

Only its context-construction prefix is compiled from the checked-in AST; no
fixture module, psql, certificates, sockets or cloud resources are loaded.
"""
import ast
from pathlib import Path
import ssl
from types import SimpleNamespace
import unittest


SOURCE = Path(__file__).with_name('test_pilot_startup_phase.py')


def context_setup():
    tree = ast.parse(SOURCE.read_text(encoding='utf-8'), filename=str(SOURCE))
    server = next(node for node in tree.body
                  if isinstance(node, ast.ClassDef) and node.name == '_StartupServer')
    serve = next(node for node in server.body
                 if isinstance(node, ast.FunctionDef) and node.name == '_serve')
    # Locate the actual adjacent context setup in the connection's with-body.
    # Stop before load_cert_chain: certificates and the server are unnecessary
    # to verify protocol selection and the configured minimum version.
    plain = next(node for node in ast.walk(serve)
                 if isinstance(node, ast.With) and any(
                     isinstance(item.context_expr, ast.Name) and item.context_expr.id == 'plain'
                     for item in node.items))
    prefix = []
    for node in plain.body:
        if not prefix:
            if (isinstance(node, ast.Assign) and isinstance(node.value, ast.Call)
                    and isinstance(node.value.func, ast.Attribute)
                    and isinstance(node.value.func.value, ast.Name)
                    and node.value.func.value.id == 'ssl'
                    and node.value.func.attr == 'SSLContext'):
                prefix.append(node)
        elif isinstance(node, ast.Assign):
            prefix.append(node)
        else:
            break
    if not prefix:
        raise AssertionError('startup context construction not found')
    return compile(ast.Module(body=prefix, type_ignores=[]), str(SOURCE), 'exec')


class StartupTlsPolicyTests(unittest.TestCase):
    def test_real_context_has_tls12_floor(self):
        namespace = {'ssl': ssl}
        exec(context_setup(), namespace)
        context = namespace['context']
        self.assertEqual(context.protocol, ssl.PROTOCOL_TLS_SERVER)
        self.assertGreaterEqual(context.minimum_version, ssl.TLSVersion.TLSv1_2)

    def test_floor_overrides_older_runtime_defaults(self):
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.minimum_version = ssl.TLSVersion.MINIMUM_SUPPORTED
        protocols = []

        def legacy_default(protocol):
            protocols.append(protocol)
            return context

        namespace = {'ssl': SimpleNamespace(SSLContext=legacy_default,
                     PROTOCOL_TLS_SERVER=ssl.PROTOCOL_TLS_SERVER, TLSVersion=ssl.TLSVersion)}
        exec(context_setup(), namespace)
        self.assertEqual(protocols, [ssl.PROTOCOL_TLS_SERVER])
        self.assertIs(namespace['context'], context)
        self.assertGreaterEqual(context.minimum_version, ssl.TLSVersion.TLSv1_2)


if __name__ == '__main__':
    unittest.main()
