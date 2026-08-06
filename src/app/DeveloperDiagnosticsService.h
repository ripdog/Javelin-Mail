#pragma once

#include "app/DeveloperDiagnostics.h"

#include <QObject>
#include <QThreadPool>

namespace javelin::app
{

    class DeveloperDiagnosticsService final : public QObject, public DeveloperDiagnosticsPort
    {
        Q_OBJECT

      public:
        DeveloperDiagnosticsService(QString databasePath, QString vaultPath,
                                    QObject* parent = nullptr);
        ~DeveloperDiagnosticsService() override;

        [[nodiscard]] QCoro::Task<DeveloperDiagnosticsResult> snapshot() override;

      private:
        QString m_databasePath;
        QString m_vaultPath;
        QThreadPool m_scanPool;
    };

} // namespace javelin::app
