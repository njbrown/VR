#include "changematerialpropertycommand.h"
#include "../irisgl/src/core/property.h"
#include "../irisgl/src/materials/custommaterial.h"


ChangeMaterialPropertyCommand::ChangeMaterialPropertyCommand(iris::CustomMaterialPtr material, QString name, QVariant oldValue, QVariant newValue)
{
    this->material = material;
    propName = name;
    this->newValue = newValue;
    this->oldValue = oldValue;
}

void ChangeMaterialPropertyCommand::undo()
{
    setMaterialProperty(propName, oldValue);
}

void ChangeMaterialPropertyCommand::redo()
{
    setMaterialProperty(propName, newValue);
}

void ChangeMaterialPropertyCommand::setMaterialProperty(QString name, QVariant value)
{
    iris::Property* prop = nullptr;
    for (auto property : material->properties) {
        if (property->name == name)
            prop = property;
    }

    Q_ASSERT(prop);

    // special case for textures since we have to generate these
    if (prop->type == iris::PropertyType::Texture) {
        material->setTextureWithUniform(prop->uniform, value.toString());
    } else {
        prop->setValue(value);
    }
}
