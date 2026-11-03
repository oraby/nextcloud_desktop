/*
 * SPDX-FileCopyrightText: 2026 Nextcloud GmbH and Nextcloud contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "syncenginetestutils.h"

#include "common/vfs.h"
#include "hydrationjob.h"
#include "vfs/openvfs/vfs_openvfs.h"
#include "vfs/openvfs/xattr.h"

#include <openvfs/openvfs.h>

#include <QtTest>

using namespace OCC;
using namespace Qt::StringLiterals;

using OpenVfsPinState = ::OpenVFS::Constants::PinStates;

namespace
{

// runs the plugin on the plain sync folder, without mounting the OpenVFS filesystem on top of it
class UnmountedOpenVfs : public OpenVFS
{
protected:
    void startImpl([[maybe_unused]] const VfsSetupParams &params) override
    {
    }
};

bool itemInstruction(const ItemCompletedSpy &spy, const QString &path, const SyncInstructions instr)
{
    auto item = spy.findItem(path);
    return item->_instruction == instr;
}

SyncJournalFileRecord dbRecord(FakeFolder &folder, const QString &path)
{
    SyncJournalFileRecord record;
    [[maybe_unused]] const auto result = folder.syncJournal().getFileRecord(path, &record);
    return record;
}

bool isDehydrated(FakeFolder &folder, Vfs &vfs, const QString &path)
{
    return vfs.isDehydratedPlaceholder(folder.localPath() + path) && dbRecord(folder, path)._type == ItemTypeVirtualFile;
}

// adds the placeholder attributes that the OpenVFS filesystem gives to items created through the mount
bool addMountAttributes(FakeFolder &folder, const QString &path, OpenVfsPinState pinState)
{
    auto attributes = ::OpenVFS::PlaceHolderAttributes::create(FileSystem::toFilesystemPath(folder.localPath() + path), {}, {}, 0);
    attributes.state = ::OpenVFS::Constants::States::Hydrated;
    attributes.pinState = pinState;
    const auto data = attributes.toData();
    return FileSystem::Xattr::setxattr(attributes.absolutePath,
                                       QString::fromUtf8(::OpenVFS::Constants::XAttributeNames::Data),
                                       {reinterpret_cast<const char *>(data.data()), static_cast<qsizetype>(data.size())})
        .isValid();
}

QSharedPointer<Vfs> setupVfs(FakeFolder &folder)
{
    // the pin states of all items inherit from the sync folder root
    ENFORCE(addMountAttributes(folder, {}, OpenVfsPinState::Unspecified));

    auto vfs = QSharedPointer<Vfs>(new UnmountedOpenVfs);
    QObject::connect(&folder.syncEngine().syncFileStatusTracker(), &SyncFileStatusTracker::fileStatusChanged, vfs.data(), &Vfs::fileStatusChanged);
    folder.switchToVfs(vfs);
    return vfs;
}

}

class TestSyncOpenVfs : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testSetPinState_onlineOnlyFolder_dehydratesFilesInside_data()
    {
        QTest::addColumn<OpenVfsPinState>("filePinState");

        // a file created through the mount and uploaded
        QTest::newRow("unspecified") << OpenVfsPinState::Unspecified;
        QTest::newRow("inherited") << OpenVfsPinState::Inherited;
        QTest::newRow("always local") << OpenVfsPinState::AlwaysLocal;
    }

    void testSetPinState_onlineOnlyFolder_dehydratesFilesInside()
    {
        QFETCH(OpenVfsPinState, filePinState);

        FakeFolder fakeFolder{FileInfo{}};
        auto vfs = setupVfs(fakeFolder);
        fakeFolder.localModifier().mkdir(u"online"_s);
        fakeFolder.localModifier().mkdir(u"online/sub"_s);
        QVERIFY(addMountAttributes(fakeFolder, u"online"_s, OpenVfsPinState::Inherited));
        QVERIFY(addMountAttributes(fakeFolder, u"online/sub"_s, OpenVfsPinState::Inherited));
        for (const auto &path : {u"online/file"_s, u"online/sub/file"_s, u"file"_s}) {
            fakeFolder.localModifier().insert(path, 64);
            QVERIFY(addMountAttributes(fakeFolder, path, filePinState));
        }
        QVERIFY(fakeFolder.syncOnce());
        QCOMPARE(fakeFolder.currentLocalState(), fakeFolder.currentRemoteState());

        ItemCompletedSpy completeSpy(fakeFolder);
        QVERIFY(vfs->setPinState(u"online"_s, PinState::OnlineOnly));
        QVERIFY(fakeFolder.syncOnce());

        for (const auto &path : {u"online/file"_s, u"online/sub/file"_s}) {
            QVERIFY(isDehydrated(fakeFolder, *vfs, path));
            QVERIFY(itemInstruction(completeSpy, path, CSYNC_INSTRUCTION_SYNC));
            QCOMPARE(completeSpy.findItem(path)->_type, ItemTypeVirtualFileDehydration);
        }
        // files outside the folder keep their content
        QCOMPARE(dbRecord(fakeFolder, u"file"_s)._type, ItemTypeFile);
        QVERIFY(!vfs->isDehydratedPlaceholder(fakeFolder.localPath() + u"file"_s));
        QCOMPARE(QFileInfo(fakeFolder.localPath() + u"file"_s).size(), 64);
    }

    void testSetPinState_alwaysLocalFolder_hydratesFilesInside()
    {
        FakeFolder fakeFolder{FileInfo{}};
        auto vfs = setupVfs(fakeFolder);
        fakeFolder.remoteModifier().mkdir(u"local"_s);
        fakeFolder.remoteModifier().mkdir(u"local/sub"_s);
        fakeFolder.remoteModifier().insert(u"local/file"_s, 64);
        fakeFolder.remoteModifier().insert(u"local/sub/file"_s, 64);
        QVERIFY(fakeFolder.syncOnce());
        QVERIFY(addMountAttributes(fakeFolder, u"local"_s, OpenVfsPinState::Inherited));
        QVERIFY(addMountAttributes(fakeFolder, u"local/sub"_s, OpenVfsPinState::Inherited));
        QVERIFY(isDehydrated(fakeFolder, *vfs, u"local/file"_s));
        QVERIFY(isDehydrated(fakeFolder, *vfs, u"local/sub/file"_s));

        ItemCompletedSpy completeSpy(fakeFolder);
        QVERIFY(vfs->setPinState(u"local"_s, PinState::AlwaysLocal));
        QVERIFY(fakeFolder.syncOnce());

        for (const auto &path : {u"local/file"_s, u"local/sub/file"_s}) {
            QCOMPARE(dbRecord(fakeFolder, path)._type, ItemTypeFile);
            QCOMPARE(QFileInfo(fakeFolder.localPath() + path).size(), 64);
            QVERIFY(itemInstruction(completeSpy, path, CSYNC_INSTRUCTION_SYNC));
        }
    }

    void testSetPinState_folderWithExcludedItems_keepsTheirPinState()
    {
        FakeFolder fakeFolder{FileInfo{}};
        auto vfs = setupVfs(fakeFolder);
        fakeFolder.localModifier().mkdir(u"folder"_s);
        fakeFolder.localModifier().insert(u"folder/excluded"_s);
        fakeFolder.localModifier().mkdir(u"folder/excludedFolder"_s);
        fakeFolder.localModifier().insert(u"folder/excludedFolder/file"_s);
        QVERIFY(addMountAttributes(fakeFolder, u"folder"_s, OpenVfsPinState::Inherited));
        QVERIFY(addMountAttributes(fakeFolder, u"folder/excluded"_s, OpenVfsPinState::Excluded));
        QVERIFY(addMountAttributes(fakeFolder, u"folder/excludedFolder"_s, OpenVfsPinState::Excluded));
        QVERIFY(addMountAttributes(fakeFolder, u"folder/excludedFolder/file"_s, OpenVfsPinState::Unspecified));

        QVERIFY(vfs->setPinState(u"folder"_s, PinState::OnlineOnly));

        QCOMPARE(*vfs->pinState(u"folder/excluded"_s), PinState::Excluded);
        QCOMPARE(*vfs->pinState(u"folder/excludedFolder"_s), PinState::Excluded);
        QCOMPARE(*vfs->pinState(u"folder/excludedFolder/file"_s), PinState::Unspecified);
    }

    void testHydrateFile_onlineOnlyFolder_keepsFileHydrated()
    {
        FakeFolder fakeFolder{FileInfo{}};
        auto vfs = setupVfs(fakeFolder);
        fakeFolder.remoteModifier().mkdir(u"online"_s);
        fakeFolder.remoteModifier().insert(u"online/file"_s, 64);
        QVERIFY(fakeFolder.syncOnce());
        QVERIFY(addMountAttributes(fakeFolder, u"online"_s, OpenVfsPinState::Inherited));
        QVERIFY(vfs->setPinState(u"online"_s, PinState::OnlineOnly));
        QVERIFY(isDehydrated(fakeFolder, *vfs, u"online/file"_s));

        // the user opens the file
        auto *job = vfs->hydrateFile(dbRecord(fakeFolder, u"online/file"_s)._fileId, fakeFolder.localPath() + u"online/file"_s);
        QVERIFY(job);
        QSignalSpy finishedSpy(job, &HydrationJob::finished);
        job->start();
        QVERIFY(finishedSpy.wait());
        QCOMPARE(*vfs->pinState(u"online/file"_s), PinState::Unspecified);

        // the next sync does not dehydrate the file while it may still be open
        QVERIFY(fakeFolder.syncOnce());
        QCOMPARE(dbRecord(fakeFolder, u"online/file"_s)._type, ItemTypeFile);
        QCOMPARE(QFileInfo(fakeFolder.localPath() + u"online/file"_s).size(), 64);
        QCOMPARE(*vfs->pinState(u"online"_s), PinState::OnlineOnly);
    }
};

QTEST_GUILESS_MAIN(TestSyncOpenVfs)
#include "testsyncopenvfs.moc"
