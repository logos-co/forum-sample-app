#include "example_forum_backend.h"

#include <iostream>

#include <QByteArray>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QIODevice>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLatin1String>
#include <QRandomGenerator>
#include <QStandardPaths>
#include <QTextStream>
#include <QTimer>
#include <QUuid>
#include <QVariantList>

// Generated umbrella: LogosModules (behind modules()) built from
// metadata.json#dependencies — the typed `delivery_module` and `keystore_signer`
// wrappers and their typed event accessors. logos_types.h provides LogosResult
// (delivery_module's return type); logos_call_error.h provides logos::CallError,
// the out-param error keystore_signer's synchronous, direct-return calls use
// instead (it has no native LogosResult wrapping).
#include "logos_call_error.h"
#include "logos_sdk.h"
#include "logos_types.h"

// Injected by CMake from metadata.json#version. Guard so the file still compiles
// (as an "unknown" version) if the definition is ever missing.
#ifndef EXAMPLE_FORUM_VERSION
#define EXAMPLE_FORUM_VERSION "unknown"
#endif

namespace {
// One consistently-tagged line per lifecycle hook / delivery event so the
// backend's activity is easy to spot (and grep) in the host's stderr stream.
void logEvent(const std::string &what) {
  std::cerr << "[example_forum backend] " << what << std::endl;
}

// A fresh, collision-free message id (also the topic id, for topics).
QString newId() {
  return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

// Length of the keystore_signer bearer credential, in bytes: 256 bits, the
// minimum secret length keystore_signer accepts.
constexpr int kCredentialBytes = 32;

// Local-echo timestamp in the same units delivery_module reports for received
// messages: nanoseconds since the Unix epoch.
qint64 nowNs() {
  return QDateTime::currentMSecsSinceEpoch() * 1000000LL;
}

// Account bookkeeping timestamp: milliseconds since the Unix epoch. Human
// scale, and deliberately not the ns units delivery_module stamps messages
// with — these two never mix.
qint64 nowMs() {
  return QDateTime::currentMSecsSinceEpoch();
}

// Schema version stamped into accounts.json, so a future layout change can be
// recognised rather than silently misparsed.
constexpr int kAccountsVersion = 1;

// The signing algorithm every account's key is minted with.
const QLatin1String kKeyAlgorithm("secp256k1");

// Small text-file helpers: the identity files are all short, single-value
// documents, and both the credential and accounts.json need reading/writing.
QString readTextFile(const QString &path) {
  QFile f(path);
  if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
    return QString();
  return QString::fromUtf8(f.readAll()).trimmed();
}

bool writeTextFile(const QString &path, const QString &contents) {
  QFile f(path);
  if (!f.open(QIODevice::WriteOnly | QIODevice::Text))
    return false;
  QTextStream out(&f);
  out << contents;
  return true;
}
} // namespace

// A LIP-23 content topic (https://lip.logos.co/messaging/informational/23/topics.html).
// Hard-coded so every instance of this app shares one forum.
const QString ExampleForumBackend::kTopic =
    QStringLiteral("/example-forum/1/forum/proto");

ExampleForumBackend::ExampleForumBackend() {
  // Runs in the ui-host process before the context is wired.
  logEvent("ctor — backend constructed (context not yet wired)");
  // EXAMPLE_FORUM_VERSION is injected by CMake from metadata.json#version.
  setAppVersion(QStringLiteral(EXAMPLE_FORUM_VERSION));
}

ExampleForumBackend::~ExampleForumBackend() {
  logEvent("dtor — backend destroyed");
}

void ExampleForumBackend::onContextReady() {
  logEvent("onContextReady — context wired, scheduling node bootstrap");
  setTopic(kTopic);

  // createNode()/start() are synchronous and can block for a moment. Defer the
  // bootstrap to the next event-loop turn so onContextReady() returns and the
  // QML view's replica can reach its Valid state promptly. modules() stays live.
  QTimer::singleShot(0, [this]() { bootstrap(); });
}

void ExampleForumBackend::bootstrap() {
  // --- Subscribe to delivery_module events before starting the node ---------

  // Node health: surface connectionStateChanged (Connected / PartiallyConnected
  // / Disconnected) as our status string once the node is up.
  modules().delivery_module.on(
      "connectionStateChanged", [this](const QVariantList &data) {
        if (data.isEmpty())
          return;
        if (nodeReady())
          setStatus(data.at(0).toString());
      });

  // Inbound forum messages on the subscribed topic. data[2] is the raw payload
  // bytes (a JSON ForumMessage envelope); data[3] is the timestamp (qint64, ns
  // since epoch). Non-forum payloads are ignored.
  modules().delivery_module.on(
      "messageReceived", [this](const QVariantList &data) {
        if (data.size() < 4)
          return;
        ForumMessage msg;
        if (!decodeForumMessage(data.at(2).toByteArray(), msg)) {
          logEvent("ignored non-forum message on " +
                   data.at(1).toString().toStdString());
          return;
        }
        logEvent("received " + msg.type.toStdString() +
                 " id=" + msg.id.toStdString());
        emitForumMessage(msg, data.at(3).toLongLong());
      });

  // --- Open/create this install's signing accounts ---------------------------
  // Independent of the delivery node below; publish() gates on both being
  // ready (nodeReady() and a selected account).
  loadAccounts();

  // --- Create + start the node against the logos.test fleet -----------------
  // No ports specified: delivery_module defaults them to 0, so the OS assigns
  // free ports and two instances on one machine don't collide.
  const QJsonObject cfg{
      {"logLevel", "INFO"},
      {"mode", "Core"},
      {"preset", "logos.test"},
  };
  const QString cfgJson =
      QString::fromUtf8(QJsonDocument(cfg).toJson(QJsonDocument::Compact));

  LogosResult created = modules().delivery_module.createNode(cfgJson);
  if (created.success) {
    logEvent("createNode succeeded, starting node");
    LogosResult started = modules().delivery_module.start();
    if (!started.success)
      logEvent("start failed: " + started.getError().toStdString());
  } else {
    // delivery_module is a singleton shared across Basecamp apps, so another
    // app may have already created and started the node. createNode then fails;
    // proceed to subscribe so we still receive on our topic.
    logEvent("createNode failed (node may already be running): " +
             created.getError().toStdString());
  }

  LogosResult subscribed = modules().delivery_module.subscribe(kTopic);
  if (!subscribed.success) {
    setStatus(QStringLiteral("subscribe failed: %1").arg(subscribed.getError()));
    logEvent("subscribe failed: " + subscribed.getError().toStdString());
    return;
  }

  setNodeReady(true);
  setStatus(QStringLiteral("Connected to forum on %1").arg(kTopic));
  logEvent("node ready — forum on " + kTopic.toStdString());
}

QString ExampleForumBackend::identityDir() const {
  // Basecamp's --user-dir gives an instance its own data tree (plugins,
  // modules, module_data, logs) and exports it to every child process — this
  // backend's ui-host included — as LOGOS_USER_DIR. keystore_signer keeps its
  // keys inside that tree (<user dir>/module_data/keystore_signer/<instance>),
  // so our credential + accounts have to live there too: an account stored
  // outside it is reloaded against a keystore that has never seen its key, and
  // every sign() then fails. Sit next to the keystore we're bound to.
  const QString userDir = qEnvironmentVariable("LOGOS_USER_DIR");
  if (!userDir.isEmpty())
    return userDir + QStringLiteral("/module_data/example_forum/identity");

  // Default launch (no --user-dir): AppDataLocation is keyed off the *host*
  // process's org/app name (the ui-host, not this plugin), so namespace under
  // it by module name to avoid colliding with any other Logos module's local
  // data. This path doesn't track the keystore's data tree either, but
  // loadAccounts() reconciles against the keystore, so a mismatch costs the
  // account labels rather than every publish().
  return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
         QStringLiteral("/example_forum/identity");
}

bool ExampleForumBackend::ensureCredential() {
  const QString dir = identityDir();
  QDir().mkpath(dir);
  const QString credentialFile =
      dir + QStringLiteral("/keystore_signer_credential");

  QByteArray credential =
      QByteArray::fromHex(readTextFile(credentialFile).toLatin1());
  if (credential.size() == kCredentialBytes) {
    m_credential = credential;
    return true;
  }
  if (!credential.isEmpty())
    logEvent("credential in " + dir.toStdString() +
             " is malformed — generating a new one");

  // A random 256-bit credential. Not a user secret, but isolation plumbing
  // (see loadAccounts()'s header doc comment). Replacing it means a brand-new
  // keystore namespace, so any accounts.json beside it reconciles to empty and
  // a fresh account is minted below — the same outcome as a first run.
  credential = QByteArray(kCredentialBytes, 0);
  for (int i = 0; i < credential.size(); ++i)
    credential[i] =
        static_cast<char>(QRandomGenerator::global()->generate() & 0xFF);

  if (!writeTextFile(credentialFile,
                     QString::fromLatin1(credential.toHex()))) {
    logEvent("failed to persist credential to " + dir.toStdString());
    return false;
  }
  m_credential = credential;
  return true;
}

void ExampleForumBackend::loadAccounts() {
  if (!ensureCredential())
    return; // no credential, no accounts — publish() reports "Identity not ready"

  const QString dir = identityDir();
  const QString accountsFile = dir + QStringLiteral("/accounts.json");

  // --- What we last persisted: the labels and the selection -----------------
  // The keystore holds the keys but knows nothing about names or which one the
  // user was posting under, so that half comes from disk.
  QVector<Account> persisted;
  QString selected;
  const QJsonDocument doc =
      QJsonDocument::fromJson(readTextFile(accountsFile).toUtf8());
  if (doc.isObject()) {
    const QJsonObject obj = doc.object();
    selected = obj.value(QStringLiteral("selected")).toString();
    const QJsonArray stored = obj.value(QStringLiteral("accounts")).toArray();
    for (const QJsonValue &value : stored) {
      const QJsonObject entry = value.toObject();
      Account acct;
      acct.keyId = entry.value(QStringLiteral("keyId")).toString();
      acct.label = entry.value(QStringLiteral("label")).toString();
      acct.createdAt = static_cast<qint64>(
          entry.value(QStringLiteral("createdAt")).toDouble());
      if (!acct.keyId.isEmpty())
        persisted.append(acct);
    }
  } else {
    // No accounts.json. A pre-accounts install still has the single key_id
    // file, so adopt that key as the first account — minting a second identity
    // beside one the user has already been posting under would silently change
    // their author id. The legacy file is left alone from here on.
    const QString legacyKeyId = readTextFile(dir + QStringLiteral("/key_id"));
    if (!legacyKeyId.isEmpty()) {
      Account acct;
      acct.keyId = legacyKeyId;
      acct.label = QStringLiteral("Account 1");
      acct.createdAt = nowMs();
      persisted.append(acct);
      selected = legacyKeyId;
      logEvent("migrating legacy identity " + legacyKeyId.toStdString() +
               " into accounts.json");
    }
  }

  // --- Reconcile against the keystore, which owns the real key set ----------
  logos::CallError err;
  const QStringList live =
      modules().keystore_signer.listKeys(m_credential, &err);

  m_accounts.clear();
  if (!err.ok()) {
    // The *call* failed, as opposed to reporting an empty namespace, so `live`
    // says nothing about what exists. Trusting it would drop every account on
    // a transient keystore hiccup, so validate each persisted account
    // individually instead — the same publicKey() probe the single-identity
    // path used, just applied per account.
    logEvent("listKeys failed (" + err.code + ": " + err.message +
             ") — validating persisted accounts individually");
    for (const Account &acct : persisted) {
      logos::CallError probe;
      if (!modules()
               .keystore_signer.publicKey(m_credential, acct.keyId, &probe)
               .isEmpty())
        m_accounts.append(acct);
      else
        logEvent("dropping account " + acct.keyId.toStdString() +
                 " — keystore holds no such key");
    }
  } else {
    // Keys that still exist keep their persisted label and order...
    for (const Account &acct : persisted) {
      if (live.contains(acct.keyId))
        m_accounts.append(acct);
      else
        logEvent("dropping account " + acct.keyId.toStdString() +
                 " — keystore no longer holds it");
    }
    // ...and keys the keystore holds that we have no record of get adopted, so
    // a lost or corrupt accounts.json heals into labelled accounts instead of
    // stranding usable keys and minting duplicates next to them.
    for (const QString &keyId : live) {
      if (indexOfAccount(keyId) >= 0)
        continue;
      Account acct;
      acct.keyId = keyId;
      acct.label = defaultAccountLabel();
      acct.createdAt = nowMs();
      m_accounts.append(acct);
      logEvent("adopted unlabelled keystore key " + keyId.toStdString() +
               " as \"" + acct.label.toStdString() + "\"");
    }
  }

  // --- First run, or nothing survived: mint the starting account ------------
  if (m_accounts.isEmpty()) {
    QString keyId;
    const QString error = mintAccount(defaultAccountLabel(), &keyId);
    if (!error.isEmpty()) {
      logEvent("could not mint a first account: " + error.toStdString());
      return; // m_keyId stays empty; publish() fails closed on it
    }
    selected = keyId;
  }

  // publishAccountState() re-points the selection if `selected` names an
  // account that didn't survive reconciliation.
  m_keyId = selected;
  publishAccountState();
  saveAccounts();
  logEvent("accounts ready — " + std::to_string(m_accounts.size()) +
           " account(s), signing as " + m_keyId.toStdString());
}

QString ExampleForumBackend::mintAccount(const QString &label,
                                         QString *outKeyId) {
  logos::CallError err;
  const QString keyId =
      modules().keystore_signer.createKey(m_credential, kKeyAlgorithm, &err);
  if (keyId.isEmpty()) {
    // Same empty-return convention as sign() (see publish()): keystore_signer
    // has no result envelope, so err is normally blank even on failure.
    // Substitute a description rather than letting "" reach the view, which
    // reads "" as success.
    const std::string detail =
        err.message.empty() ? std::string("keystore_signer returned no key id")
                            : err.message;
    logEvent("createKey failed: " + detail);
    return QString::fromStdString("Couldn't create account: " + detail);
  }

  Account acct;
  acct.keyId = keyId;
  acct.label = label;
  acct.createdAt = nowMs();
  m_accounts.append(acct);
  if (outKeyId)
    *outKeyId = keyId;
  logEvent("minted account " + keyId.toStdString() + " (\"" +
           label.toStdString() + "\")");
  return QString();
}

bool ExampleForumBackend::saveAccounts() {
  QJsonArray stored;
  for (const Account &acct : m_accounts) {
    QJsonObject entry;
    entry.insert(QStringLiteral("keyId"), acct.keyId);
    entry.insert(QStringLiteral("label"), acct.label);
    entry.insert(QStringLiteral("createdAt"), acct.createdAt);
    stored.append(entry);
  }
  QJsonObject root;
  root.insert(QStringLiteral("version"), kAccountsVersion);
  root.insert(QStringLiteral("selected"), m_keyId);
  root.insert(QStringLiteral("accounts"), stored);

  const QString path = identityDir() + QStringLiteral("/accounts.json");
  if (!writeTextFile(path, QString::fromUtf8(QJsonDocument(root).toJson(
                               QJsonDocument::Indented)))) {
    // In-memory state stands, so the session keeps working — only the
    // selection and labels are lost on the next start.
    logEvent("failed to persist accounts to " + path.toStdString());
    return false;
  }
  return true;
}

void ExampleForumBackend::publishAccountState() {
  // m_keyId must always name a held account, or be empty when there are none.
  // Every mutation lands here, so this is the one place that re-establishes
  // that invariant — including after a delete removed the selected account.
  if (indexOfAccount(m_keyId) < 0)
    m_keyId = m_accounts.isEmpty() ? QString() : m_accounts.first().keyId;

  QJsonArray view;
  for (const Account &acct : m_accounts) {
    QJsonObject entry;
    entry.insert(QStringLiteral("keyId"), acct.keyId);
    entry.insert(QStringLiteral("label"), acct.label);
    view.append(entry);
  }
  setAccountsJson(QString::fromUtf8(
      QJsonDocument(view).toJson(QJsonDocument::Compact)));

  const int selectedIndex = indexOfAccount(m_keyId);
  setMyLabel(selectedIndex >= 0 ? m_accounts.at(selectedIndex).label
                                : QString());
  // myAddress last: the view gates composing on it, so it should only go
  // non-empty once the list and label it refers to are already published.
  setMyAddress(m_keyId);
}

int ExampleForumBackend::indexOfAccount(const QString &keyId) const {
  if (keyId.isEmpty())
    return -1;
  for (int i = 0; i < m_accounts.size(); ++i)
    if (m_accounts.at(i).keyId == keyId)
      return i;
  return -1;
}

QString ExampleForumBackend::defaultAccountLabel() const {
  // Count up from the current size, skipping names already in use so deleting
  // "Account 2" of three doesn't hand the next account a name that's still on
  // screen. At most m_accounts.size() names are taken, so this terminates.
  for (int n = m_accounts.size() + 1;; ++n) {
    const QString candidate = QStringLiteral("Account %1").arg(n);
    bool taken = false;
    for (const Account &acct : m_accounts) {
      if (acct.label == candidate) {
        taken = true;
        break;
      }
    }
    if (!taken)
      return candidate;
  }
}

QString ExampleForumBackend::createAccount(QString label) {
  if (m_credential.isEmpty())
    return QStringLiteral("Accounts aren't ready yet");

  label = label.trimmed();
  if (label.isEmpty())
    label = defaultAccountLabel();

  QString keyId;
  const QString error = mintAccount(label, &keyId);
  if (!error.isEmpty())
    return error;

  // Select what was just created — creating an account and then continuing to
  // post as the old one would be surprising.
  m_keyId = keyId;
  publishAccountState();
  saveAccounts();
  return QString(); // empty == success
}

QString ExampleForumBackend::selectAccount(QString keyId) {
  if (indexOfAccount(keyId) < 0)
    return QStringLiteral("No such account");
  if (keyId == m_keyId)
    return QString(); // already signing as this one

  m_keyId = keyId;
  publishAccountState();
  saveAccounts();
  logEvent("selected account " + keyId.toStdString());
  return QString();
}

QString ExampleForumBackend::renameAccount(QString keyId, QString label) {
  const int index = indexOfAccount(keyId);
  if (index < 0)
    return QStringLiteral("No such account");
  label = label.trimmed();
  if (label.isEmpty())
    return QStringLiteral("An account needs a name");

  // Display metadata only — the key is untouched, so this changes neither what
  // was signed before nor what can be signed after.
  m_accounts[index].label = label;
  publishAccountState();
  saveAccounts();
  return QString();
}

QString ExampleForumBackend::deleteAccount(QString keyId) {
  const int index = indexOfAccount(keyId);
  if (index < 0)
    return QStringLiteral("No such account");
  if (m_accounts.size() == 1)
    return QStringLiteral("Can't delete your only account");

  // Destroy the key first: dropping the account while the keystore still held
  // its key would strand a usable key that the next reconcile would silently
  // re-adopt as an unlabelled account.
  logos::CallError err;
  const bool deleted =
      modules().keystore_signer.deleteKey(m_credential, keyId, &err);
  if (!deleted || !err.ok()) {
    const std::string detail =
        err.message.empty()
            ? "keystore_signer did not delete key " + keyId.toStdString()
            : err.message;
    logEvent("deleteKey failed: " + detail);
    return QString::fromStdString("Couldn't delete account: " + detail);
  }

  const std::string label = m_accounts.at(index).label.toStdString();
  m_accounts.removeAt(index);
  // publishAccountState() re-selects when this was the selected account.
  publishAccountState();
  saveAccounts();
  logEvent("deleted account " + keyId.toStdString() + " (\"" + label + "\")");
  return QString();
}

QString ExampleForumBackend::createTopic(QString title, QString body) {
  if (title.isEmpty())
    return QStringLiteral("A topic needs a title");

  ForumMessage msg;
  msg.type = QStringLiteral("topic");
  msg.id = topicIdFor(title); // content-addressed: derived from the title
  msg.title = title;
  msg.body = body;
  return publish(msg);
}

QString ExampleForumBackend::reconstructTopic(QString topicId, QString title) {
  // Local recovery for a topic we only know through its replies (a backfilled
  // placeholder in the view). The topic id is a hash of the title, so a title
  // shared out-of-band and pasted here is provably the right one iff its hash
  // matches. No network send — we just surface the verified topic locally, with
  // an empty body (the body isn't part of the id and can't be recovered from
  // it; it fills in later only if the original topic message reaches us).
  if (title.isEmpty())
    return QStringLiteral("Enter the topic title");
  if (topicIdFor(title) != topicId)
    return QStringLiteral("That title doesn't match this topic");

  // No author: this is a local-only reconstruction from a title, not a
  // signed message that arrived over the network.
  emit topicReceived(topicId, title, QString(), QString(), nowNs());
  return QString(); // empty == success
}

QString ExampleForumBackend::replyToTopic(QString topicId, QString body) {
  if (topicId.isEmpty())
    return QStringLiteral("No topic selected");
  if (body.isEmpty())
    return QStringLiteral("A reply needs a body");

  ForumMessage msg;
  msg.type = QStringLiteral("reply");
  msg.id = newId();
  msg.topicId = topicId;
  msg.body = body;
  return publish(msg);
}

QString ExampleForumBackend::publish(ForumMessage msg) {
  // Unconditional entry log — the two early-return guards below fail closed
  // and silently (no send, no local echo, so nothing reaches the topics/reply
  // list), so this is what distinguishes "never got here" from "got here and
  // one of the guards tripped" when diagnosing a post that doesn't appear.
  logEvent("publish(" + msg.type.toStdString() + " id=" + msg.id.toStdString() +
           "): contextReady=" + std::to_string(isContextReady()) +
           " nodeReady=" + std::to_string(nodeReady()) +
           " myKeyId=" + (m_keyId.isEmpty() ? "<empty>" : m_keyId.toStdString()));

  if (!isContextReady() || !nodeReady())
    return QStringLiteral("Node not ready");
  if (m_keyId.isEmpty())
    return QStringLiteral("Identity not ready");

  // Sign the canonical payload (Keccak-256, matching go-wallet-sdk's
  // go-ethereum-derived signing convention) before encoding — author/sig must
  // be set on msg itself so encodeForumMessage() carries them on the wire.
  msg.author = m_keyId;
  const QByteArray signingBytes = forumMessageSigningBytes(msg);
  const QByteArray hash = QCryptographicHash::hash(signingBytes,
                                                    QCryptographicHash::Keccak_256);

  // Sign with keystore-signer-module. Unlike accounts_module, keystore-signer
  // provides per-caller isolation via the secret credential, so no re-init
  // workaround is needed. The sign() call returns raw signature bytes.
  logos::CallError err;
  const QByteArray sigBytes =
      modules().keystore_signer.sign(m_credential, m_keyId, hash, &err);
  if (sigBytes.isEmpty()) {
    // keystore_signer reports failure by returning empty bytes, not an error
    // (its .lidl has no result envelope), so err.message is normally empty
    // here. Substitute a real description: an empty return would otherwise
    // reach the view as "" — which the view reads as success — silently
    // dropping the post instead of showing why it failed.
    const std::string detail =
        err.message.empty()
            ? "keystore_signer returned no signature for key " + m_keyId.toStdString() +
                  " (is this identity still in its keystore?)"
            : err.message;
    logEvent("sign failed: " + detail);
    return QString::fromStdString("sign failed: " + detail);
  }
  // Convert signature bytes to hex string for JSON wire format.
  msg.sig = QStringLiteral("0x") + QString::fromLatin1(sigBytes.toHex());

  LogosResult r = modules().delivery_module.send(kTopic, encodeForumMessage(msg));
  if (!r.success) {
    logEvent("send failed: " + r.getError().toStdString());
    return r.getError();
  }
  logEvent("published " + msg.type.toStdString() + " id=" + msg.id.toStdString() +
           ", requestId=" + r.getString().toStdString());

  // Local echo — the relay won't loop our own message back, so surface it now.
  // The id lets the QML view de-dupe if the network ever does echo it.
  emitForumMessage(msg, nowNs());
  return QString(); // empty == success
}

void ExampleForumBackend::emitForumMessage(const ForumMessage &msg,
                                           qint64 timestamp) {
  if (msg.type == QLatin1String("topic"))
    emit topicReceived(msg.id, msg.title, msg.body, msg.author, timestamp);
  else if (msg.type == QLatin1String("reply"))
    emit replyReceived(msg.id, msg.topicId, msg.body, msg.author, timestamp);
}
