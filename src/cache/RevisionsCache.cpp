#include "RevisionsCache.h"

#include <QLogger.h>

using namespace QLogger;

RevisionsCache::RevisionsCache(QObject *parent)
   : QObject(parent)
{
}

RevisionsCache::~RevisionsCache()
{
   for (auto commit : mCommits)
      delete commit;

   mCommits.clear();
   mCommitsMap.clear();
   mReferences.clear();
}

void RevisionsCache::beginCacheConfig(int numElementsToStore)
{
   QMutexLocker locker(&mMutex);

   QLog_Debug("Git", QString("Configuring the cache for {%1} elements.").arg(numElementsToStore));

   if (!mCacheLocked)
   {
      if (mCommits.isEmpty())
      {
         // We reserve 1 extra slots for the ZERO_SHA (aka WIP commit)
         mCommits.resize(numElementsToStore + 1);
         mCommitsMap.reserve(numElementsToStore + 1);
      }

      mCacheLocked = true;
   }
}

void RevisionsCache::endCacheConfig()
{
   QMutexLocker locker(&mMutex);

   mCacheLocked = false;
}

void RevisionsCache::commit(CommitInfo c, const QString &localBranch)
{
   QMutexLocker locker(&mMutex);

   c.setLanes(calculateLanes(c));

   const auto commit = new CommitInfo(std::move(c));

   mCommits.insert(1, commit);

   mCommitsMap.insert(commit->sha(), commit);
   mCommitsMap.insert(c.sha(), commit);

   const auto parent = mCommitsMap.value(c.parent(0));
   auto branches = parent->getReferences(References::Type::LocalBranch);

   if (branches.contains(localBranch))
      parent->removeReference(References::Type::LocalBranch, localBranch);

   commit->addReference(References::Type::LocalBranch, localBranch);
}

void RevisionsCache::updateCommitSha(const QString &oldSha, CommitInfo c)
{
   QMutexLocker locker(&mMutex);

   const auto commit = mCommitsMap[oldSha];
   c.addReferences(commit->getAllReferences());
   *commit = c;

   mCommitsMap.remove(oldSha);
   mCommitsMap.insert(c.sha(), commit);
}

void RevisionsCache::clearLanes()
{
   mLanes.clear();
}

void RevisionsCache::updateLanes()
{
   QMutexLocker locker(&mMutex);

   for (const auto commit : mCommits)
      if (commit != mCommits[0])
         commit->setLanes(calculateLanes(*commit));
}

CommitInfo RevisionsCache::getCommitInfoByRow(int row) const
{
   if (mCacheLocked)
   {
      QLog_Info("Git", QString("The cache is updating!"));
      return CommitInfo();
   }

   const auto commit = row >= 0 && row < mCommits.count() ? mCommits.at(row) : nullptr;

   return commit ? *commit : CommitInfo();
}

int RevisionsCache::getCommitPos(const QString &sha)
{
   if (mCacheLocked)
   {
      QLog_Info("Git", QString("The cache is updating!"));
      return -1;
   }

   QMutexLocker locker(&mMutex);

   const auto commit = mCommitsMap.value(sha, nullptr);
   return mCommits.indexOf(commit);
}

CommitInfo RevisionsCache::getCommitInfoByField(CommitInfo::Field field, const QString &text, int startingPoint)
{
   if (mCacheLocked)
   {
      QLog_Info("Git", QString("The cache is updating!"));
      return CommitInfo();
   }

   auto commitIter = searchCommit(field, text, startingPoint);

   if (commitIter == mCommits.constEnd() && startingPoint > 0)
      commitIter = searchCommit(field, text);

   return commitIter != mCommits.constEnd() ? **commitIter : CommitInfo();
}

CommitInfo RevisionsCache::getCommitInfo(const QString &sha)
{
   if (mCacheLocked)
   {
      QLog_Info("Git", QString("The cache is updating!"));
      return CommitInfo();
   }

   QMutexLocker locker(&mMutex);

   if (!sha.isEmpty())
   {
      const auto c = mCommitsMap.value(sha, nullptr);

      if (c == nullptr)
      {
         const auto shas = mCommitsMap.keys();
         const auto it = std::find_if(shas.cbegin(), shas.cend(),
                                      [sha](const QString &shaToCompare) { return shaToCompare.startsWith(sha); });

         if (it != shas.cend())
            return *mCommitsMap.value(*it);

         return CommitInfo();
      }

      return *c;
   }

   return CommitInfo();
}

RevisionFiles RevisionsCache::getRevisionFile(const QString &sha1, const QString &sha2) const
{
   if (mCacheLocked)
   {
      QLog_Info("Git", QString("The cache is updating!"));
      return RevisionFiles();
   }

   return mRevisionFilesMap.value(qMakePair(sha1, sha2));
}

void RevisionsCache::insertCommitInfo(CommitInfo rev, int orderIdx)
{
   if (mCacheLocked)
   {
      if (mCommitsMap.contains(rev.sha()))
         QLog_Info("Git", QString("The commit with SHA {%1} is already in the cache.").arg(rev.sha()));
      else
      {
         rev.setLanes(calculateLanes(rev));

         const auto commit = new CommitInfo(rev);

         if (orderIdx >= mCommits.count())
         {
            QLog_Debug("Git", QString("Adding commit with sha {%1}.").arg(commit->sha()));

            mCommits.append(commit);
         }
         else if (!(mCommits[orderIdx] && *mCommits[orderIdx] == *commit))
         {
            QLog_Trace("Git", QString("Overwriting commit with sha {%1}.").arg(commit->sha()));

            if (mCommits[orderIdx])
               delete mCommits[orderIdx];

            mCommits[orderIdx] = commit;
         }

         mCommitsMap.insert(rev.sha(), commit);

         if (mCommitsMap.contains(rev.parent(0)))
            mCommitsMap.remove(rev.parent(0));
      }
   }
   else
      QLog_Info("Git", QString("The cache is updating!"));
}

bool RevisionsCache::insertRevisionFile(const QString &sha1, const QString &sha2, const RevisionFiles &file)
{
   if (mCacheLocked)
   {
      QLog_Info("Git", QString("The cache is updating!"));
      return false;
   }

   const auto key = qMakePair(sha1, sha2);

   if (!sha1.isEmpty() && !sha2.isEmpty() && mRevisionFilesMap.value(key) != file)
   {
      QLog_Debug("Git", QString("Adding the revisions files between {%1} and {%2}.").arg(sha1, sha2));

      mRevisionFilesMap.insert(key, file);

      return true;
   }

   return false;
}

void RevisionsCache::insertReference(const QString &sha, References::Type type, const QString &reference)
{
   if (mCacheLocked)
   {
      QLog_Debug("Git", QString("Adding a new reference with SHA {%1}.").arg(sha));

      auto commit = mCommitsMap[sha];

      if (commit)
      {
         commit->addReference(type, reference);

         if (!mReferences.contains(mCommitsMap[sha]))
            mReferences.append(mCommitsMap[sha]);
      }
   }
   else
      QLog_Info("Git", QString("The cache is updating!"));
}

void RevisionsCache::insertLocalBranchDistances(const QString &name, const LocalBranchDistances &distances)
{
   if (mCacheLocked)
      mLocalBranchDistances[name] = distances;
   else
      QLog_Info("Git", QString("The cache is updating!"));
}

RevisionsCache::LocalBranchDistances RevisionsCache::getLocalBranchDistances(const QString &name)
{
   if (mCacheLocked)
   {
      QLog_Info("Git", QString("The cache is updating!"));
      return LocalBranchDistances();
   }

   return mLocalBranchDistances.value(name);
}

void RevisionsCache::updateWipCommit(const QString &parentSha, const QString &diffIndex, const QString &diffIndexCache)
{
   QMutexLocker locker(&mMutex);

   QLog_Debug("Git", QString("Updating the WIP commit. The actual parent has SHA {%1}.").arg(parentSha));

   const auto key = qMakePair(CommitInfo::ZERO_SHA, parentSha);
   const auto fakeRevFile = fakeWorkDirRevFile(diffIndex, diffIndexCache);

   insertRevisionFile(CommitInfo::ZERO_SHA, parentSha, fakeRevFile);

   const QString longLog;
   const auto author = QString("-");
   const auto log
       = fakeRevFile.count() == mUntrackedfiles.count() ? QString("No local changes") : QString("Local changes");
   CommitInfo c(CommitInfo::ZERO_SHA, { parentSha }, author, QDateTime::currentDateTime().toSecsSinceEpoch(), log,
                longLog);

   // if (mLanes.isEmpty())
   mLanes.init(c.sha());

   c.setLanes(calculateLanes(c));
   /*
      if (mCommits[0])
         c.setLanes(mCommits[0]->getLanes());
   */
   const auto sha = c.sha();
   const auto commit = new CommitInfo(std::move(c));

   if (mCommits[0])
      delete mCommits[0];

   mCommits[0] = commit;

   mCommitsMap.insert(sha, commit);
}

void RevisionsCache::removeReference(const QString &sha)
{
   if (mCacheLocked)
   {
      QLog_Info("Git", QString("The cache is updating!"));
      return;
   }

   QMutexLocker locker(&mMutex);

   mCommitsMap[sha]->addReferences(References());
}

bool RevisionsCache::containsRevisionFile(const QString &sha1, const QString &sha2) const
{
   if (mCacheLocked)
   {
      QLog_Info("Git", QString("The cache is updating!"));
      return false;
   }

   return mRevisionFilesMap.contains(qMakePair(sha1, sha2));
}

QVector<Lane> RevisionsCache::calculateLanes(const CommitInfo &c)
{
   const auto sha = c.sha();

   QLog_Trace("Git", QString("Updating the lanes for SHA {%1}.").arg(sha));

   bool isDiscontinuity;
   bool isFork = mLanes.isFork(sha, isDiscontinuity);
   bool isMerge = c.parentsCount() > 1;

   if (isDiscontinuity)
      mLanes.changeActiveLane(sha); // uses previous isBoundary state

   if (isFork)
      mLanes.setFork(sha);
   if (isMerge)
      mLanes.setMerge(c.parents());
   if (c.parentsCount() == 0)
      mLanes.setInitial();

   const auto lanes = mLanes.getLanes();

   resetLanes(c, isFork);

   return lanes;
}

RevisionFiles RevisionsCache::parseDiffFormat(const QString &buf, FileNamesLoader &fl)
{
   RevisionFiles rf;
   auto parNum = 1;
   const auto lines = buf.split("\n", QString::SkipEmptyParts);

   for (auto line : lines)
   {
      if (line[0] == ':') // avoid sha's in merges output
      {
         if (line[1] == ':')
         { // it's a combined merge
            /* For combined merges rename/copy information is useless
             * because nor the original file name, nor similarity info
             * is given, just the status tracks that in the left/right
             * branch a renamed/copy occurred (as example status could
             * be RM or MR). For visualization purposes we could consider
             * the file as modified
             */
            if (fl.rf != &rf)
            {
               flushFileNames(fl);
               fl.rf = &rf;
            }
            appendFileName(line.section('\t', -1), fl);
            rf.setStatus("M");
            rf.mergeParent.append(parNum);
         }
         else
         {
            if (line.at(98) == '\t') // Faster parsing in normal case
            {
               if (fl.rf != &rf)
               {
                  flushFileNames(fl);
                  fl.rf = &rf;
               }
               appendFileName(line.mid(99), fl);
               rf.setStatus(line.at(97));
               rf.mergeParent.append(parNum);
            }
            else // It's a rename or a copy, we are not in fast path now!
               setExtStatus(rf, line.mid(97), parNum, fl);
         }
      }
      else
         ++parNum;
   }

   return rf;
}

void RevisionsCache::appendFileName(const QString &name, FileNamesLoader &fl)
{
   int idx = name.lastIndexOf('/') + 1;
   const QString &dr = name.left(idx);
   const QString &nm = name.mid(idx);

   auto it = mDirNames.indexOf(dr);
   if (it == -1)
   {
      int idx = mDirNames.count();
      mDirNames.append(dr);
      fl.rfDirs.append(idx);
   }
   else
      fl.rfDirs.append(it);

   it = mFileNames.indexOf(nm);
   if (it == -1)
   {
      int idx = mFileNames.count();
      mFileNames.append(nm);
      fl.rfNames.append(idx);
   }
   else
      fl.rfNames.append(it);

   fl.files.append(name);
}

void RevisionsCache::flushFileNames(FileNamesLoader &fl)
{
   if (!fl.rf)
      return;

   for (auto i = 0; i < fl.rfNames.count(); ++i)
   {
      const auto dirName = mDirNames.at(fl.rfDirs.at(i));
      const auto fileName = mFileNames.at(fl.rfNames.at(i));

      if (!fl.rf->mFiles.contains(dirName + fileName))
         fl.rf->mFiles.append(dirName + fileName);
   }

   fl.rfNames.clear();
   fl.rfDirs.clear();
   fl.rf = nullptr;
}

bool RevisionsCache::pendingLocalChanges()
{
   auto localChanges = false;

   if (!mCacheLocked)
   {
      QMutexLocker locker(&mMutex);

      const auto commit = mCommitsMap.value(CommitInfo::ZERO_SHA);

      if (commit)
      {
         const auto rf = getRevisionFile(CommitInfo::ZERO_SHA, commit->parent(0));
         localChanges = rf.count() == mUntrackedfiles.count();
      }
   }
   else
      QLog_Info("Git", QString("The cache is updating!"));

   return localChanges;
}

QVector<QPair<QString, QStringList>> RevisionsCache::getBranches(References::Type type)
{
   QVector<QPair<QString, QStringList>> branches;

   if (!mCacheLocked)
   {
      QMutexLocker locker(&mMutex);

      for (auto commit : mReferences)
         branches.append(QPair<QString, QStringList>(commit->sha(), commit->getReferences(type)));
   }
   else
      QLog_Info("Git", QString("The cache is updating!"));

   return branches;
}

QVector<QPair<QString, QStringList>> RevisionsCache::getTags()
{
   QVector<QPair<QString, QStringList>> tags;

   if (!mCacheLocked)
   {
      QMutexLocker locker(&mMutex);

      for (auto commit : mReferences)
         tags.append(QPair<QString, QStringList>(commit->sha(), commit->getReferences(References::Type::Tag)));
   }
   else
      QLog_Info("Git", QString("The cache is updating!"));

   return tags;
}

QString RevisionsCache::getCommitForBranch(const QString &branch, bool local)
{
   QString sha;

   if (!mCacheLocked)
   {
      QMutexLocker locker(&mMutex);

      for (auto commit : mReferences)
      {
         const auto branches
             = commit->getReferences(local ? References::Type::LocalBranch : References::Type::RemoteBranches);

         if (branches.contains(branch))
         {
            sha = commit->sha();
            break;
         }
      }
   }
   else
      QLog_Info("Git", QString("The cache is updating!"));

   return sha;
}

void RevisionsCache::setExtStatus(RevisionFiles &rf, const QString &rowSt, int parNum, FileNamesLoader &fl)
{
   const QStringList sl(rowSt.split('\t', QString::SkipEmptyParts));
   if (sl.count() != 3)
      return;

   // we want store extra info with format "orig --> dest (Rxx%)"
   // but git give us something like "Rxx\t<orig>\t<dest>"
   QString type = sl[0];
   type.remove(0, 1);
   const QString &orig = sl[1];
   const QString &dest = sl[2];
   const QString extStatusInfo(orig + " --> " + dest + " (" + QString::number(type.toInt()) + "%)");

   /*
    NOTE: we set rf.extStatus size equal to position of latest
          copied/renamed file. So it can have size lower then
          rf.count() if after copied/renamed file there are
          others. Here we have no possibility to know final
          dimension of this RefFile. We are still in parsing.
 */

   // simulate new file
   if (fl.rf != &rf)
   {
      flushFileNames(fl);
      fl.rf = &rf;
   }
   appendFileName(dest, fl);
   rf.mergeParent.append(parNum);
   rf.setStatus(RevisionFiles::NEW);
   rf.appendExtStatus(extStatusInfo);

   // simulate deleted orig file only in case of rename
   if (type.at(0) == 'R')
   { // renamed file
      if (fl.rf != &rf)
      {
         flushFileNames(fl);
         fl.rf = &rf;
      }
      appendFileName(orig, fl);
      rf.mergeParent.append(parNum);
      rf.setStatus(RevisionFiles::DELETED);
      rf.appendExtStatus(extStatusInfo);
   }
   rf.setOnlyModified(false);
}

QVector<CommitInfo *>::const_iterator RevisionsCache::searchCommit(CommitInfo::Field field, const QString &text,
                                                                   const int startingPoint) const
{
   return std::find_if(mCommits.constBegin() + startingPoint, mCommits.constEnd(),
                       [field, text](CommitInfo *info) { return info->getFieldStr(field).contains(text); });
}

void RevisionsCache::resetLanes(const CommitInfo &c, bool isFork)
{
   const auto nextSha = c.parentsCount() == 0 ? QString() : c.parent(0);

   mLanes.nextParent(nextSha);

   if (c.parentsCount() > 1)
      mLanes.afterMerge();
   if (isFork)
      mLanes.afterFork();
   if (mLanes.isBranch())
      mLanes.afterBranch();
}

void RevisionsCache::clear()
{
   QMutexLocker locker(&mMutex);

   mDirNames.clear();
   mFileNames.clear();
   mRevisionFilesMap.clear();
   mLanes.clear();
   mCommitsMap.clear();
   mReferences.clear();
}

int RevisionsCache::count() const
{
   return mCommits.count();
}

RevisionFiles RevisionsCache::fakeWorkDirRevFile(const QString &diffIndex, const QString &diffIndexCache)
{
   FileNamesLoader fl;
   auto rf = parseDiffFormat(diffIndex, fl);
   fl.rf = &rf;
   rf.setOnlyModified(false);

   for (const auto &it : qAsConst(mUntrackedfiles))
   {
      if (fl.rf != &rf)
      {
         flushFileNames(fl);
         fl.rf = &rf;
      }

      appendFileName(it, fl);
      rf.setStatus(RevisionFiles::UNKNOWN);
      rf.mergeParent.append(1);
   }

   RevisionFiles cachedFiles = parseDiffFormat(diffIndexCache, fl);
   flushFileNames(fl);

   for (auto i = 0; i < rf.count(); i++)
   {
      if (cachedFiles.mFiles.indexOf(rf.getFile(i)) != -1)
      {
         if (cachedFiles.statusCmp(i, RevisionFiles::CONFLICT))
            rf.appendStatus(i, RevisionFiles::CONFLICT);

         rf.appendStatus(i, RevisionFiles::IN_INDEX);
      }
   }

   return rf;
}

RevisionFiles RevisionsCache::parseDiff(const QString &logDiff)
{
   FileNamesLoader fl;

   auto rf = parseDiffFormat(logDiff, fl);
   fl.rf = &rf;
   flushFileNames(fl);

   return rf;
}

void RevisionsCache::setUntrackedFilesList(const QVector<QString> &untrackedFiles)
{
   mUntrackedfiles = untrackedFiles;
}
