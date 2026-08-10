#pragma once

#include <QObject>

class NoRttiAccessibleObject final : public QObject
{
  public:
    NoRttiAccessibleObject();
    ~NoRttiAccessibleObject() override;
};
