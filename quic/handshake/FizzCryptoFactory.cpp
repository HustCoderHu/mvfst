/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 *
 */

#include <quic/handshake/FizzCryptoFactory.h>

#include <quic/handshake/FizzBridge.h>
#include <quic/handshake/FizzPacketNumberCipher.h>
#include <quic/handshake/HandshakeLayer.h>

namespace {

class QuicPlaintextReadRecordLayer : public fizz::PlaintextReadRecordLayer {
 public:
  ~QuicPlaintextReadRecordLayer() override = default;

  folly::Optional<fizz::TLSMessage> read(folly::IOBufQueue& buf) override {
    if (buf.empty()) {
      return folly::none;
    }
    fizz::TLSMessage msg;
    msg.type = fizz::ContentType::handshake;
    msg.fragment = buf.move();
    return msg;
  }
};

class QuicEncryptedReadRecordLayer : public fizz::EncryptedReadRecordLayer {
 public:
  ~QuicEncryptedReadRecordLayer() override = default;

  explicit QuicEncryptedReadRecordLayer(fizz::EncryptionLevel encryptionLevel)
      : fizz::EncryptedReadRecordLayer(encryptionLevel) {}

  folly::Optional<fizz::TLSMessage> read(folly::IOBufQueue& buf) override {
    if (buf.empty()) {
      return folly::none;
    }
    fizz::TLSMessage msg;
    msg.type = fizz::ContentType::handshake;
    msg.fragment = buf.move();
    return msg;
  }
};

class QuicPlaintextWriteRecordLayer : public fizz::PlaintextWriteRecordLayer {
 public:
  ~QuicPlaintextWriteRecordLayer() override = default;

  fizz::TLSContent write(fizz::TLSMessage&& msg) const override {
    fizz::TLSContent content;
    content.data = std::move(msg.fragment);
    content.contentType = msg.type;
    content.encryptionLevel = getEncryptionLevel();
    return content;
  }

  fizz::TLSContent writeInitialClientHello(
      std::unique_ptr<folly::IOBuf> encodedClientHello) const override {
    return write(fizz::TLSMessage{fizz::ContentType::handshake,
                                  std::move(encodedClientHello)});
  }
};

class QuicEncryptedWriteRecordLayer : public fizz::EncryptedWriteRecordLayer {
 public:
  ~QuicEncryptedWriteRecordLayer() override = default;

  explicit QuicEncryptedWriteRecordLayer(fizz::EncryptionLevel encryptionLevel)
      : EncryptedWriteRecordLayer(encryptionLevel) {}

  fizz::TLSContent write(fizz::TLSMessage&& msg) const override {
    fizz::TLSContent content;
    content.data = std::move(msg.fragment);
    content.contentType = msg.type;
    content.encryptionLevel = getEncryptionLevel();
    return content;
  }
};

} // namespace

namespace quic {

Buf FizzCryptoFactory::makeInitialTrafficSecret(
    folly::StringPiece label,
    const ConnectionId& clientDestinationConnId,
    QuicVersion version) const {
  auto deriver = makeKeyDeriver(fizz::CipherSuite::TLS_AES_128_GCM_SHA256);
  auto connIdRange = folly::range(clientDestinationConnId);
  folly::StringPiece salt;
  switch (version) {
    case QuicVersion::MVFST_OLD:
      salt = kQuicDraft17Salt;
      break;
    case QuicVersion::MVFST:
    case QuicVersion::QUIC_DRAFT_22:
      salt = kQuicDraft22Salt;
      break;
    case QuicVersion::QUIC_DRAFT:
      salt = kQuicDraft23Salt;
      break;
    default:
      // Default to one arbitrarily.
      salt = kQuicDraft17Salt;
  }
  auto initialSecret = deriver->hkdfExtract(salt, connIdRange);
  auto trafficSecret = deriver->expandLabel(
      folly::range(initialSecret),
      label,
      folly::IOBuf::create(0),
      fizz::Sha256::HashLen);
  return trafficSecret;
}

std::unique_ptr<Aead> FizzCryptoFactory::makeInitialAead(
    folly::StringPiece label,
    const ConnectionId& clientDestinationConnId,
    QuicVersion version) const {
  auto trafficSecret =
      makeInitialTrafficSecret(label, clientDestinationConnId, version);
  auto deriver = makeKeyDeriver(fizz::CipherSuite::TLS_AES_128_GCM_SHA256);
  auto aead = makeAead(fizz::CipherSuite::TLS_AES_128_GCM_SHA256);
  auto key = deriver->expandLabel(
      trafficSecret->coalesce(),
      kQuicKeyLabel,
      folly::IOBuf::create(0),
      aead->keyLength());
  auto iv = deriver->expandLabel(
      trafficSecret->coalesce(),
      kQuicIVLabel,
      folly::IOBuf::create(0),
      aead->ivLength());

  fizz::TrafficKey trafficKey = {std::move(key), std::move(iv)};
  aead->setKey(std::move(trafficKey));
  return FizzAead::wrap(std::move(aead));
}

std::unique_ptr<PacketNumberCipher> FizzCryptoFactory::makePacketNumberCipher(
    folly::ByteRange baseSecret) const {
  auto pnCipher =
      makePacketNumberCipher(fizz::CipherSuite::TLS_AES_128_GCM_SHA256);
  auto deriver = makeKeyDeriver(fizz::CipherSuite::TLS_AES_128_GCM_SHA256);
  auto pnKey = deriver->expandLabel(
      baseSecret, kQuicPNLabel, folly::IOBuf::create(0), pnCipher->keyLength());
  pnCipher->setKey(pnKey->coalesce());
  return pnCipher;
}

std::unique_ptr<fizz::PlaintextReadRecordLayer>
FizzCryptoFactory::makePlaintextReadRecordLayer() const {
  return std::make_unique<QuicPlaintextReadRecordLayer>();
}

std::unique_ptr<fizz::PlaintextWriteRecordLayer>
FizzCryptoFactory::makePlaintextWriteRecordLayer() const {
  return std::make_unique<QuicPlaintextWriteRecordLayer>();
}

std::unique_ptr<fizz::EncryptedReadRecordLayer>
FizzCryptoFactory::makeEncryptedReadRecordLayer(
    fizz::EncryptionLevel encryptionLevel) const {
  return std::make_unique<QuicEncryptedReadRecordLayer>(encryptionLevel);
}

std::unique_ptr<fizz::EncryptedWriteRecordLayer>
FizzCryptoFactory::makeEncryptedWriteRecordLayer(
    fizz::EncryptionLevel encryptionLevel) const {
  return std::make_unique<QuicEncryptedWriteRecordLayer>(encryptionLevel);
}

std::unique_ptr<PacketNumberCipher> FizzCryptoFactory::makePacketNumberCipher(
    fizz::CipherSuite cipher) const {
  switch (cipher) {
    case fizz::CipherSuite::TLS_AES_128_GCM_SHA256:
      return std::make_unique<Aes128PacketNumberCipher>();
    case fizz::CipherSuite::TLS_AES_256_GCM_SHA384:
      return std::make_unique<Aes256PacketNumberCipher>();
    default:
      throw std::runtime_error("Packet number cipher not implemented");
  }
}

} // namespace quic
