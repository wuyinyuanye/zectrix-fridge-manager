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
    kSeafood,
    kRice,
    kTofu,
    kBread,
    kSauce,
    kRoot,
};

struct FoodHint {
    std::string zone = "冷藏";
    int days = 5;  // suggested storage days (a guideline, not an expiry); 0 = unknown
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
    if (ContainsAny(name, {"冰淇淋", "雪糕", "冰棍", "冰块", "冰沙"})) return FoodIcon::kSnow;
    if (ContainsAny(name, {"酱", "生抽", "老抽", "醋", "料酒", "蚝油", "沙拉", "豆瓣", "香油", "食用油"})) return FoodIcon::kSauce;
    if (ContainsAny(name, {"豆腐", "豆干", "腐竹", "千张", "豆皮", "素鸡"})) return FoodIcon::kTofu;
    if (ContainsAny(name, {"面包", "吐司", "蛋糕", "蛋挞", "馒头", "包子", "三明治", "可颂", "贝果", "饼"})) return FoodIcon::kBread;
    if (ContainsAny(name, {"奶", "酸奶", "芝士", "黄油", "奶油", "奶酪"})) return FoodIcon::kMilk;
    if (ContainsAny(name, {"蛋", "鹌鹑蛋", "皮蛋", "咸蛋"})) return FoodIcon::kEgg;
    if (ContainsAny(name, {"虾", "鱼", "蟹", "贝", "蛤", "鱿", "三文鱼", "海鲜", "鳗", "鲍", "螺"})) return FoodIcon::kSeafood;
    if (ContainsAny(name, {"米饭", "剩饭", "粥", "面条", "拉面", "意面", "水饺", "饺子", "馄饨", "抄手", "汤圆", "年糕", "饭"})) return FoodIcon::kRice;
    if (ContainsAny(name, {"肉", "排骨", "牛", "羊", "猪", "鸡", "鸭", "鹅", "火腿", "培根", "香肠", "腊肠", "丸"})) return FoodIcon::kMeat;
    if (ContainsAny(name, {"水", "茶", "可乐", "雪碧", "果汁", "汁", "咖啡", "啤酒", "酒", "豆浆", "饮料", "汽水"})) return FoodIcon::kDrink;
    if (ContainsAny(name, {"果", "苹果", "梨", "橙", "橘", "柚", "莓", "葡萄", "西瓜", "哈密瓜", "桃", "李", "樱桃", "香蕉", "芒果", "猕猴桃", "柠檬", "圣女果", "番茄", "西红柿", "蓝莓"})) return FoodIcon::kFruit;
    if (ContainsAny(name, {"胡萝卜", "萝卜", "土豆", "马铃薯", "红薯", "地瓜", "山药", "芋头", "冬瓜", "南瓜", "苦瓜", "丝瓜", "黄瓜", "茄子", "莲藕", "藕", "玉米", "洋葱", "蘑菇", "香菇", "金针菇", "木耳"})) return FoodIcon::kRoot;
    if (zone == "冷冻") return FoodIcon::kSnow;
    return FoodIcon::kLeaf;
}

// Built-in Chinese kitchen food knowledge base (100+ common items)
inline FoodHint Lookup(const std::string& name) {
    FoodHint h;
    // Explicitly frozen goods ("冻虾", "速冻虎虾", "冻鸡腿"), but not 果冻 (jelly)
    if (name.find("冻") != std::string::npos && name.find("果冻") == std::string::npos) {
        h.zone = "冷冻"; h.days = 30; h.icon = GuessIcon(name, "冷冻");
        return h;
    }
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
    h.days = 0;  // unknown food: no suggested storage days
    return h;
}

// ============================================================================
// Recommended home storage times, used ONLY for the "X已经放了N天啦" reminder.
// Source: USDA FSIS / FoodSafety.gov "Cold Food Storage Chart" and the USDA FoodKeeper
// data (refrigerator <= 4C / 40F, freezer -18C / 0F). Where the source gives a range
// we use the LOWER end. Freezer times are for QUALITY only (frozen food stays safe).
// 0 = the source gives no figure (or says "do not freeze"): no reminder.
// Items checked in order: more specific names first ("鸡蛋" before "鸡", "牛奶" before "牛").
// ============================================================================
struct StorageGuide {
    int fridge_days = 0;
    int freezer_days = 0;
};

inline StorageGuide LookupStorageGuide(const std::string& name) {
    struct Rule {
        std::initializer_list<const char*> keys;
        int fridge_days;
        int freezer_days;
    };
    static const Rule kRules[] = {
        // Not covered by the source (look like covered foods by name)
        {{"牛油果", "鱼丸", "鱼豆腐", "蟹棒", "肉松", "牛肉干", "牛轧", "鸡精", "果冻",
          "皮蛋", "咸蛋", "咸鸭蛋", "蛋糕", "蛋挞", "汤圆", "水饺", "饺子", "馄饨", "包子",
          "腊肉", "腊肠", "卤", "猪肝", "鸡肝", "腰花", "肚", "淡奶油"}, 0, 0},
        // Eggs: raw in shell 3-5 weeks, do not freeze; hard-boiled 1 week
        {{"水煮蛋", "卤蛋", "茶叶蛋", "煮鸡蛋"}, 7, 0},
        {{"蛋"}, 21, 0},
        // Dairy: yogurt 1-2 wk / 1-2 mo; milk = package date / 3 mo; butter 1-2 mo / 6-9 mo;
        // cheese soft 1-2 wk, hard opened 3-4 wk / 6 mo
        {{"酸奶"}, 7, 30},
        {{"牛奶", "鲜奶", "纯奶", "巴氏奶"}, 0, 90},
        {{"黄油"}, 30, 180},
        {{"奶酪", "芝士"}, 7, 180},
        // Processed meat: bacon 1 wk / 1 mo; cooked sausage & hot dogs 1 wk / 1-2 mo;
        // ham & luncheon meat (opened) 3-5 days / 1-2 mo
        {{"培根"}, 7, 30},
        {{"香肠", "热狗"}, 7, 30},
        {{"火腿", "午餐肉"}, 3, 30},
        // Ground meat 1-2 days / 3-4 months
        {{"肉馅", "肉末", "绞肉"}, 1, 90},
        // Raw poultry: whole 1-2 days / 1 year; pieces 1-2 days / 9 months
        {{"整鸡", "整只鸡"}, 1, 365},
        {{"鸡", "鸭", "鹅"}, 1, 270},
        // Shrimp, crayfish, scallops 1-3 days / 6-18 months
        {{"虾", "扇贝"}, 1, 180},
        // Fatty fish 1-2 days / 2-3 months; lean fish 1-2 days / 6-8 months
        {{"三文鱼", "鲑鱼", "金枪鱼", "鲭鱼", "青花鱼", "秋刀鱼"}, 1, 60},
        {{"鱼"}, 1, 180},
        // Fresh beef, pork, lamb steaks/chops/roasts 3-5 days / 4-12 months
        {{"排骨", "五花肉", "里脊", "牛排", "肥牛", "羊肉卷", "猪", "牛", "羊"}, 3, 120},
        // Leftovers & soups 3-4 days; frozen 1-2 months (no-meat) .. 2-6 months (meat)
        {{"剩菜", "剩饭", "米饭", "汤", "炖", "红烧", "炒"}, 3, 30},
        // Tofu 1 week / 5 months
        {{"豆腐"}, 7, 150},
        // Mushrooms 3-7 days / 10-12 months
        {{"蘑菇", "香菇", "平菇", "金针菇", "杏鲍菇", "口蘑", "菌"}, 3, 300},
        // Leafy greens 3-7 days, not recommended for freezing
        {{"菠菜", "生菜", "油麦菜", "小白菜", "青菜", "上海青", "空心菜", "茼蒿", "菜心"}, 3, 0},
        // Broccoli 3-5 days / 10-12 months; cucumber 4-6 days / no; carrots 2-3 weeks / 10-12 months
        {{"西兰花"}, 3, 300},
        {{"黄瓜"}, 4, 0},
        {{"胡萝卜"}, 14, 300},
        // Fruit: strawberries 2-3 days / 8-12 mo; blueberries 1-2 wk / 8-12 mo;
        // grapes 1 wk / 1 mo; bananas 3 days / 2-3 mo
        {{"草莓"}, 2, 240},
        {{"蓝莓"}, 7, 240},
        {{"葡萄", "提子"}, 7, 30},
        {{"香蕉"}, 3, 60},
    };
    for (const auto& r : kRules) {
        if (ContainsAny(name, r.keys)) return {r.fridge_days, r.freezer_days};
    }
    return {};
}

}  // namespace food_catalog

#endif  // ZECTRIX_FRIDGE_FOOD_CATALOG_H_
