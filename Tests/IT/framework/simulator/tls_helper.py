"""Generate a self-signed TLS certificate + private key for local testing."""

import ipaddress
import datetime
import os

from cryptography import x509
from cryptography.hazmat.backends import default_backend
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import rsa
from cryptography.x509.oid import NameOID


def generate_self_signed_cert(output_dir: str) -> tuple[str, str]:
    """Generate a self-signed cert/key pair valid for 127.0.0.1.

    Returns (certfile_path, keyfile_path).
    """
    key = rsa.generate_private_key(
        public_exponent=65537,
        key_size=2048,
        backend=default_backend(),
    )

    subject = issuer = x509.Name([
        x509.NameAttribute(NameOID.COMMON_NAME, "127.0.0.1"),
    ])

    cert = (
        x509.CertificateBuilder()
        .subject_name(subject)
        .issuer_name(issuer)
        .public_key(key.public_key())
        .serial_number(x509.random_serial_number())
        .not_valid_before(datetime.datetime.utcnow())
        .not_valid_after(datetime.datetime.utcnow() + datetime.timedelta(days=1))
        .add_extension(
            x509.SubjectAlternativeName([
                x509.IPAddress(ipaddress.IPv4Address("127.0.0.1")),
            ]),
            critical=False,
        )
        .sign(key, hashes.SHA256(), default_backend())
    )

    certfile = os.path.join(output_dir, "test_cert.pem")
    keyfile = os.path.join(output_dir, "test_key.pem")

    with open(certfile, "wb") as f:
        f.write(cert.public_bytes(serialization.Encoding.PEM))

    with open(keyfile, "wb") as f:
        f.write(key.private_bytes(
            serialization.Encoding.PEM,
            serialization.PrivateFormat.TraditionalOpenSSL,
            serialization.NoEncryption(),
        ))

    return certfile, keyfile
