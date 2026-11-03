/*
 * SPDX-FileCopyrightText: 2026 Nextcloud GmbH and Nextcloud contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "syncenginetestutils.h"

#include "filesystem.h"
#include "hydrationjob.h"

#include <QTemporaryDir>
#include <QtTest>

using namespace OCC;
using namespace Qt::StringLiterals;

class TestHydrationJob : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testStart_downloadSucceeds_fileIsCompleteWhenFinished_data()
    {
        QTest::addColumn<qint64>("fileSize");

        // smaller and larger than the write buffer of QFile
        QTest::newRow("small") << qint64{4};
        QTest::newRow("large") << qint64{1024 * 1024 + 7};
    }

    void testStart_downloadSucceeds_fileIsCompleteWhenFinished()
    {
        QFETCH(qint64, fileSize);

        FakeFolder fakeFolder{FileInfo{}};
        fakeFolder.remoteModifier().insert(u"file"_s, fileSize);
        QVERIFY(fakeFolder.syncOnce());

        SyncJournalFileRecord record;
        QVERIFY(fakeFolder.syncJournal().getFileRecord("file"_ba, &record));
        QVERIFY(record.isValid());
        QCOMPARE(record._fileSize, fileSize);

        QTemporaryDir targetDir;
        QVERIFY(targetDir.isValid());
        const auto targetPath = targetDir.filePath(u"file"_s);

        auto job =
            std::make_unique<HydrationJob>(fakeFolder.syncEngine().syncOptions()._vfs.data(), record._fileId, std::make_unique<QFile>(targetPath), nullptr);
        job->setTargetFile(targetPath);

        auto sizeWhenFinished = qint64{-1};
        connect(job.get(), &HydrationJob::finished, this, [&] {
            sizeWhenFinished = QFileInfo(targetPath).size();
            // restore the server modification time, as the VFS plugin does once the hydration finished
            QVERIFY(FileSystem::setModTime(targetPath, record._modtime));
        });
        QSignalSpy finishedSpy(job.get(), &HydrationJob::finished);
        QSignalSpy errorSpy(job.get(), &HydrationJob::error);

        job->start();
        QVERIFY(finishedSpy.wait());
        QCOMPARE(errorSpy.count(), 0);

        // all downloaded data is on disk by the time the result handlers run
        QCOMPARE(sizeWhenFinished, fileSize);

        // destroying the job must not write to the file again and change its modification time
        job.reset();
        QCOMPARE(QFileInfo(targetPath).size(), fileSize);
        QCOMPARE(static_cast<qint64>(FileSystem::getModTime(targetPath)), record._modtime);
    }
};

QTEST_GUILESS_MAIN(TestHydrationJob)
#include "testhydrationjob.moc"
