#ifndef ZECTRIX_FRIDGE_FOOD_CATALOG_H_
#define ZECTRIX_FRIDGE_FOOD_CATALOG_H_

#include <string>

enum class FoodIcon : uint8_t {
    kLeaf = 0,
    kMilk,
    kMeat,
    kEgg,
    kFruit,
    kDrink,
    kSnow,
};

struct FoodHint {
    std::string zone = "冷藏";
    int days = 5;
    FoodIcon icon = FoodIcon::kLeaf;
    std::string tip;
};

namespace food_catalog {

inline bool ContainsAny(const std::string& s, std::initializer_list<const char*> keys) {
    for (const char* k : keys) {
        if (s.find(k) != std::string::npos) return true;
    }
    return false;
}

inline bool IsPantryStaple(const std::string& name) {
    return ContainsAny(name, {
        "盐", "食用油", "植物油", "花生油", "菜籽油", "橄榄油", "香油", "麻油",
        "生抽", "老抽", "酱油", "醋", "料酒", "白糖", "冰糖", "红糖", "淀粉",
        "味精", "鸡精", "蚝油", "胡椒", "花椒", "八角", "桂皮", "香叶", "干辣椒",
        "清水", "开水", "冷水", "热水", "葱花", "姜丝", "蒜末"
    });
}

inline FoodIcon GuessIcon(const std::string& name, const std::string& zone) {
    if (zone == "冷冻") return FoodIcon::kSnow;
    if (ContainsAny(name, {"奶", "酸奶", "芝士", "黄油", "奶油", "奶酪"})) return FoodIcon::kMilk;
    if (ContainsAny(name, {"蛋", "鹌鹑蛋", "皮蛋", "咸蛋"})) return FoodIcon::kEgg;
    if (ContainsAny(name, {"肉", "排骨", "牛", "羊", "猪", "鸡", "鸭", "鹅", "鱼", "虾", "蟹", "贝", "火腿", "培根", "香肠", "丸"})) return FoodIcon::kMeat;
    if (ContainsAny(name, {"果", "苹果", "梨", "橙", "橘", "柚", "莓", "葡萄", "西瓜", "哈密瓜", "桃", "李", "樱桃", "香蕉", "芒果", "猕猴桃", "柠檬", "圣女果", "番茄", "西红柿"})) return FoodIcon::kFruit;
    if (ContainsAny(name, {"水", "茶", "可乐", "雪碧", "果汁", "咖啡", "啤酒", "酒", "豆浆", "椰汁", "饮料"})) return FoodIcon::kDrink;
    return FoodIcon::kLeaf;
}

// Built-in Chinese kitchen food knowledge base (100+ common items)
inline FoodHint Lookup(const std::string& name) {
    FoodHint h;
    // Frozen items
    if (ContainsAny(name, {"水饺", "饺子", "汤圆", "馄饨", "包子", "馒头", "冰淇淋", "雪糕", "速冻", "冻肉", "牛排", "肥牛", "羊肉卷", "丸子", "排骨"})) {
        h.zone = "冷冻"; h.days = 30; h.icon = FoodIcon::kSnow;
        return h;
    }
    // Dairy
    if (ContainsAny(name, {"鲜奶", "鲜牛奶", "巴氏奶"})) {
        h.zone = "冷藏"; h.days = 5; h.icon = FoodIcon::kMilk; h.tip = "放冰箱内层更保鲜";
        return h;
    }
    if (ContainsAny(name, {"酸奶", "牛奶", "纯奶", "芝士", "奶酪", "黄油", "淡奶油"})) {
        h.zone = "冷藏"; h.days = 10; h.icon = FoodIcon::kMilk;
        return h;
    }
    // Fresh seafood & raw meat (short shelf life in fridge!)
    if (ContainsAny(name, {"虾", "鱼", "蟹", "贝", "蛤", "鱿鱼", "三文鱼", "海鲜"})) {
        h.zone = "冷藏"; h.days = 2; h.icon = FoodIcon::kMeat; h.tip = "海鲜尽量2天内吃完";
        return h;
    }
    if (ContainsAny(name, {"猪肉", "牛肉", "羊肉", "鸡胸", "鸡腿", "鸡翅", "五花肉", "里脊", "肉末", "肉馅", "猪肝", "鸭"})) {
        h.zone = "冷藏"; h.days = 3; h.icon = FoodIcon::kMeat; h.tip = "超过3天建议转冷冻";
        return h;
    }
    if (ContainsAny(name, {"火腿", "培根", "香肠", "午餐肉", "腊肉", "卤"})) {
        h.zone = "冷藏"; h.days = 7; h.icon = FoodIcon::kMeat;
        return h;
    }
    // Eggs
    if (ContainsAny(name, {"蛋"})) {
        h.zone = "冷藏"; h.days = 15; h.icon = FoodIcon::kEgg; h.tip = "大头朝上放置更保鲜";
        return h;
    }
    // Leafy veggies (short!)
    if (ContainsAny(name, {"菠菜", "生菜", "油麦菜", "空心菜", "小白菜", "青菜", "上海青", "香菜", "芹菜", "韭菜", "茼蒿", "芥兰", "豆芽", "香葱", "小葱", "葱"})) {
        h.zone = "冷藏"; h.days = 3; h.icon = FoodIcon::kLeaf; h.tip = "绿叶菜沥干包纸巾存放";
        return h;
    }
    // Tofu & mushrooms
    if (ContainsAny(name, {"豆腐", "豆干", "千张", "腐竹", "豆皮", "金针菇", "香菇", "平菇", "杏鲍菇", "口蘑", "菌"})) {
        h.zone = "冷藏"; h.days = 4; h.icon = FoodIcon::kLeaf;
        return h;
    }
    // Sturdy veggies
    if (ContainsAny(name, {"西兰花", "花菜", "白菜", "娃娃菜", "包菜", "甘蓝", "黄瓜", "茄子", "青椒", "彩椒", "辣椒", "西葫芦", "苦瓜", "丝瓜", "四季豆", "豌豆", "玉米", "莲藕", "秋葵"})) {
        h.zone = "冷藏"; h.days = 6; h.icon = FoodIcon::kLeaf;
        return h;
    }
    if (ContainsAny(name, {"萝卜", "胡萝卜", "冬瓜", "南瓜"})) {
        h.zone = "冷藏"; h.days = 12; h.icon = FoodIcon::kLeaf;
        return h;
    }
    // Room-temp friendly veggies/fruits (storage pitfall tips!)
    if (ContainsAny(name, {"土豆", "马铃薯", "红薯", "山药", "洋葱", "大蒜", "生姜", "姜"})) {
        h.zone = "常温"; h.days = 14; h.icon = FoodIcon::kLeaf; h.tip = "阴凉干燥处存放即可";
        return h;
    }
    if (ContainsAny(name, {"香蕉", "芒果", "牛油果", "木瓜"})) {
        h.zone = "常温"; h.days = 4; h.icon = FoodIcon::kFruit; h.tip = "热带水果放冷藏易发黑";
        return h;
    }
    if (ContainsAny(name, {"草莓", "蓝莓", "车厘子", "樱桃", "葡萄", "提子", "圣女果", "小番茄", "番茄", "西红柿", "桃", "西瓜"})) {
        h.zone = "冷藏"; h.days = 4; h.icon = FoodIcon::kFruit;
        return h;
    }
    if (ContainsAny(name, {"苹果", "梨", "橙", "橘", "柚", "柠檬", "猕猴桃", "哈密瓜"})) {
        h.zone = "冷藏"; h.days = 12; h.icon = FoodIcon::kFruit;
        return h;
    }
    // Bread & staples
    if (ContainsAny(name, {"面包", "吐司", "蛋糕", "甜品", "布丁"})) {
        h.zone = "常温"; h.days = 4; h.icon = FoodIcon::kDrink;
        return h;
    }
    // Drinks & sauces
    if (ContainsAny(name, {"可乐", "雪碧", "啤酒", "饮料", "果汁", "茶", "豆浆", "酱", "醋", "油"})) {
        h.zone = "冷藏"; h.days = 15; h.icon = FoodIcon::kDrink;
        return h;
    }
    // Leftovers
    if (ContainsAny(name, {"剩菜", "剩饭", "米饭", "汤", "炒", "炖"})) {
        h.zone = "冷藏"; h.days = 2; h.icon = FoodIcon::kLeaf; h.tip = "熟食尽快2天内复热吃完";
        return h;
    }
    h.icon = GuessIcon(name, h.zone);
    return h;
}

}  // namespace food_catalog

#endif  // ZECTRIX_FRIDGE_FOOD_CATALOG_H_
