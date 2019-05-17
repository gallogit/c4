//MIT License
//
//Copyright(c) 2019 Alex Kasitskyi
//
//Permission is hereby granted, free of charge, to any person obtaining a copy
//of this software and associated documentation files(the "Software"), to deal
//in the Software without restriction, including without limitation the rights
//to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
//copies of the Software, and to permit persons to whom the Software is
//furnished to do so, subject to the following conditions :
//
//The above copyright notice and this permission notice shall be included in all
//copies or substantial portions of the Software.
//
//THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
//IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
//FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.IN NO EVENT SHALL THE
//AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
//LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
//OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
//SOFTWARE.

#include <c4/jpeg.hpp>

#include <fstream>

#include <map>
#include <unordered_map>

#include <c4/math.hpp>
#include <c4/mstream.hpp>
#include <c4/logger.hpp>
#include <c4/json.hpp>
#include <c4/geometry.hpp>
#include <c4/scaling.hpp>
#include <c4/string.hpp>
#include <c4/matrix_regression.hpp>


#include <c4/image.hpp>
#include <c4/color_plane.hpp>
#include <c4/bmp24.hpp>
#include <c4/serialize.hpp>

struct LBP {
    c4::matrix<uint8_t> operator()(const c4::matrix<uint8_t>& img) const {
        c4::matrix<uint8_t> lbp(img.height() - 2, img.width() - 2);

        for (int i : c4::range(lbp.height())) {
            for (int j : c4::range(lbp.width())) {
                const c4::matrix<uint8_t> m = img.submatrix(i, j, 3, 3);
                const uint8_t c = m[1][1];

                int v = 0;

                v = (v << 1) | (m[0][0] > c);
                v = (v << 1) | (m[0][1] > c);
                v = (v << 1) | (m[0][2] > c);
                v = (v << 1) | (m[1][0] > c);
                v = (v << 1) | (m[1][2] > c);
                v = (v << 1) | (m[2][0] > c);
                v = (v << 1) | (m[2][1] > c);
                v = (v << 1) | (m[2][2] > c);

                lbp[i][j] = uint8_t(v);
            }
        }

        return lbp;
    }
};

struct LBP_2x2 {
    c4::matrix<uint8_t> operator()(const c4::matrix<uint8_t>& img) const {
        c4::matrix<uint8_t> lbp(img.height() - 5, img.width() - 5);
        
        c4::matrix<uint8_t> m(3, 3);

        for (int i : c4::range(lbp.height())) {
            for (int j : c4::range(lbp.width())) {
                c4::scale_bilinear(img.submatrix(i, j, 6, 6), m);

                const uint8_t c = m[1][1];

                int v = 0;

                v = (v << 1) | (m[0][0] > c);
                v = (v << 1) | (m[0][1] > c);
                v = (v << 1) | (m[0][2] > c);
                v = (v << 1) | (m[1][0] > c);
                v = (v << 1) | (m[1][2] > c);
                v = (v << 1) | (m[2][0] > c);
                v = (v << 1) | (m[2][1] > c);
                v = (v << 1) | (m[2][2] > c);

                lbp[i][j] = uint8_t(v);
            }
        }

        return lbp;
    }
};

template<class TForm>
void generate_samples(const c4::matrix_dimensions& sample_size, const c4::matrix<uint8_t>& img, const std::vector<c4::rectangle<int>>& rects, std::vector<c4::matrix<uint8_t>>& train_x, std::vector<float>& train_y, int k, TForm tform) {
    c4::matrix<uint8_t> sample(sample_size);

    int positive = 0;

    for (const auto& r : rects) {
        for (int dx = -k; dx <= k; dx++) {
            for (int dy = -k; dy <= k; dy++) {
                c4::rectangle<int> r1(r.x + dx, r.y + dy, r.w, r.h);

                const auto cropped_rect = r1.intersect(img.rect());
                c4::scale_bilinear(img.submatrix(cropped_rect), sample);
                train_x.push_back(tform(sample));
                train_y.push_back(1.f);

                positive++;
            }
        }
    }

    if (img.height() <= sample.height() || img.width() <= sample.width())
        return;

    c4::fast_rand rnd;
    int negatives = positive * 1;
    while (negatives--) {
        c4::rectangle<int> r;
        r.y = rnd() % (img.height() - sample.height());
        r.x = rnd() % (img.width() - sample.width());
        r.h = sample_size.height;
        r.w = sample_size.width;

        double iou_max = 0.;
        for (const auto& t : rects) {
            iou_max = std::max(iou_max, c4::intersection_over_union(r, t));
        }
        if (iou_max < 0.1) {
            train_x.push_back(tform(img.submatrix(r.y, r.x, r.h, r.w)));
            train_y.push_back(0.f);
        }
    }
}

struct dataset {
    std::vector<c4::matrix<uint8_t>> x;
    std::vector<float> y;
    const c4::matrix_dimensions sample_size;

    dataset(const c4::matrix_dimensions& sample_size) : sample_size(sample_size) {}

    void load(const std::string& labels_filepath, int k) {
        c4::fast_rand rnd;

        c4::json data_json;

        std::string root = "C:/portraits-data/ibug_300W_large_face_landmark_dataset/";
        std::ifstream train_data_fin(root + labels_filepath);

        train_data_fin >> data_json;

        for (const auto& image_info : data_json["images"]) {
            std::string filename = image_info["filename"];

            // Right now we only have jpeg input...
            if (!c4::ends_with(c4::to_lower(filename), ".jpg") || rnd() % 20)
                continue;

            c4::matrix<uint8_t> img;
            c4::read_jpeg(root + filename, img);

            const auto& boxes = image_info["boxes"];

            std::vector<c4::rectangle<int>> rects;
            for (const auto& box : boxes) {
                c4::rectangle<int> r(box["left"], box["top"], box["width"], box["height"]);
                rects.push_back(r);
            }

            generate_samples(sample_size, img, rects, x, y, k, LBP());
        }
    }
};

enum TestEnum {
    OPTIUON_1,
    OPTION_2
};

int main(int argc, char* argv[]) {
    try{
        {
            const TestEnum test_enum(OPTIUON_1);

            std::unordered_map<int, std::map<int, std::string>> test_map;
            test_map[0][1] = "a";
            std::vector<std::list<std::tuple<int, float, double>>> test_vec(1);
            test_vec[0].emplace_back(0, 0.f, 0.0);

            c4::ovstream mout;

            c4::serialize::output_archive out1(mout);

            out1(test_enum, test_map, test_vec);

            std::vector<char> vec;
            mout.swap_vector(vec);

            PRINT_DEBUG(mout.get_vector().size());

            c4::imstream min(vec.data(), vec.size());

            c4::serialize::input_archive in1(min);

            TestEnum test_enum1;
            decltype(test_map) test_map1;
            decltype(test_vec) test_vec1;
            in1(test_enum1, test_map1, test_vec1);

            ASSERT_TRUE(test_enum1 == test_enum);
            ASSERT_TRUE(test_map1 == test_map);
            ASSERT_TRUE(test_vec1 == test_vec);

            std::cout << "OK" << std::endl;
        }
        // step 20, k2:
        // 32 - 0.0463786
        // 40 - 0.0428498
        // 48 - 0.0418165
        // 56 - 0.0407401
        // 60 - 0.030433
        //*64 - 0.0264947
        // 68 - 0.034898
        // 72 - 0.0346339
        // 80 - 0.0288526

        // step 20, k3:
        // 64 - 0.0181997

        // step 20, k4:
        // 64 - 0.0141404

        // step 0, k2:
        // 64 - 0.0178519

        const c4::matrix_dimensions sample_size{ 64, 64 };
        dataset train_set(sample_size);
        train_set.load("labels_ibug_300W_train.json", 2);

        std::cout << "train size: " << train_set.y.size() << std::endl;
        std::cout << "positive ratio: " << std::accumulate(train_set.y.begin(), train_set.y.end(), 0.f) / train_set.y.size() << std::endl;

        dataset test_set(sample_size);
        test_set.load("labels_ibug_300W_test.json", 0);

        std::cout << "test size: " << test_set.y.size() << std::endl;
        std::cout << "positive ratio: " << std::accumulate(test_set.y.begin(), test_set.y.end(), 0.f) / test_set.y.size() << std::endl;

        __c4::matrix_regression<> mr;

        mr.train(train_set.x, train_set.y, test_set.x, test_set.y);

        c4::matrix<uint8_t> testm(10, 10);

        c4::ovstream vout;

        c4::serialize::output_archive out(vout);

        out(mr);

        c4::imstream min(vout.get_vector().data(), vout.get_vector().size());
        c4::serialize::input_archive in(min);

        __c4::matrix_regression<> mr1;
        in(mr1);

        const double train_mse = c4::mean_squared_error(mr1.predict(train_set.x), train_set.y);
        const double test_mse = c4::mean_squared_error(mr1.predict(test_set.x), test_set.y);

        LOGD << "RELOAD: train_mse: " << train_mse << ", test_mse: " << test_mse;
    } catch (const std::exception& e) {
        std::cerr << e.what() << std::endl;
    }
    return 0;
}
