#ifndef FORUM_UI_INTERFACE_H
#define FORUM_UI_INTERFACE_H

#include "interface.h"
#include <QObject>
#include <QString>

class ForumUiInterface : public PluginInterface {
public:
  virtual ~ForumUiInterface() = default;
};

#define ForumUiInterface_iid "org.logos.ForumUiInterface"
Q_DECLARE_INTERFACE(ForumUiInterface, ForumUiInterface_iid)

#endif // FORUM_UI_INTERFACE_H