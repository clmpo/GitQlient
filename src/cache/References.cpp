#include "References.h"

void References::addReference(Type type, const QString &value)
{
   mReferences[type].append(value);
}

QStringList References::getReferences(Type type) const
{
   return mReferences.value(type, QStringList());
}

void References::removeReference(References::Type type, const QString &value)
{
   mReferences[type].removeAll(value);
}
