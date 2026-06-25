#ifndef FORUM_UI_PLUGIN_H
#define FORUM_UI_PLUGIN_H

#include "LogosViewPluginBase.h"
#include "forum_ui_interface.h"
#include "rep_forum_ui_source.h"
#include <QString>
#include <QVariantList>

class LogosAPI;
class LogosModules;

// Inherits ForumUiSimpleSource (generated from forum_ui.rep) so
// enableRemoting() can publish the typed source and QML replicas get
// auto-synced properties + callable slots.
class ForumUiPlugin : public ForumUiSimpleSource,
                      public ForumUiInterface,
                      public ForumUiViewPluginBase {
  Q_OBJECT
  Q_PLUGIN_METADATA(IID ForumUiInterface_iid FILE "metadata.json")
  Q_INTERFACES(ForumUiInterface)

public:
  explicit ForumUiPlugin(QObject *parent = nullptr);
  ~ForumUiPlugin() override;

  QString name() const override { return "forum_ui"; }
  QString version() const override { return "1.0.0"; }

  Q_INVOKABLE void initLogos(LogosAPI *api);

  // Slots from calc_ui_cpp.rep — return values directly. The QML replica
  // receives QRemoteObjectPendingReply; use logos.watch() in QML to get the
  // value.
  int add(int a, int b) override;
  QString getVersion(int i) override;

signals:
  void eventResponse(const QString &eventName, const QVariantList &args);

private:
  LogosAPI *m_logosAPI = nullptr;
  LogosModules *m_logos = nullptr;
};

#endif // FORUM_UI_PLUGIN_H