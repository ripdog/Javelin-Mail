#pragma once

class QObject;

class NoRttiAccessibleObjectHandle final
{
  public:
    NoRttiAccessibleObjectHandle();
    ~NoRttiAccessibleObjectHandle();

    NoRttiAccessibleObjectHandle(const NoRttiAccessibleObjectHandle&) = delete;
    NoRttiAccessibleObjectHandle& operator=(const NoRttiAccessibleObjectHandle&) = delete;

    [[nodiscard]] QObject* get() const;

  private:
    QObject* m_object = nullptr;
};
