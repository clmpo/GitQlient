#pragma once

#include <QMap>
#include <QStringList>

class References
{
public:
   enum class Type
   {
      Tag,
      LocalBranch,
      RemoteBranches,
      Applied,
      UnApplied,
      AnyRef
   };

   void addReference(Type type, const QString &value);

   QStringList getReferences(Type type) const;

   void removeReference(Type type, const QString &value);

   bool isEmpty() const { return mReferences.isEmpty(); }

private:
   QMap<Type, QStringList> mReferences;
};
