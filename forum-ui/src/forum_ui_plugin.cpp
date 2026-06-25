#include "forum_ui_plugin.h"
#include "logos_api.h"
#include "logos_sdk.h"

ForumUiPlugin::ForumUiPlugin(QObject *parent) : ForumUiSimpleSource(parent) {}
ForumUiPlugin::~ForumUiPlugin() { delete m_logos; }

void ForumUiPlugin::initLogos(LogosAPI *api) {
  if (m_logos)
    return;
  m_logosAPI = api;
  m_logos = new LogosModules(api);
  // Register this object as the Remote Objects source so the QML replica
  // can see its properties and call its slots.
  setBackend(this);
}

int ForumUiPlugin::add(int a, int b) { return m_logos->forum_app.add(a, b); }

QString ForumUiPlugin::getVersion(int i) {
  switch (i) {
  case 0:
    return m_logos->forum_app.libVersion();
  case 1:
    return m_logos->forum_app.getModuleInfo();
  default:
    return version();
  }
}
