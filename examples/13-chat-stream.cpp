#include "openai.hpp"

#include <iostream>

int main() {
    openai::start();

    openai::Json input = R"(
    {
        "model": "gpt-3.5-turbo",
        "messages":[{"role":"user", "content":"Write a short haiku about the sea."}],
        "max_tokens": 64,
        "temperature": 0
    }
    )"_json;

    auto on_chunk = [](const openai::Json& chunk) -> bool {
        if (!chunk.contains("choices") || !chunk["choices"].is_array() || chunk["choices"].empty()) {
            return true;
        }
        const auto& choice = chunk["choices"][0];
        if (!choice.contains("delta") || !choice["delta"].is_object()) {
            return true;
        }
        const auto& delta = choice["delta"];
        if (delta.contains("content") && delta["content"].is_string()) {
            std::cout << delta["content"].get<std::string>() << std::flush;
        }
        return true;
    };

    auto final_response = openai::chat().createStream(input, on_chunk);
    std::cout << "\n\nFinal aggregated response:\n" << final_response.dump(2) << '\n';
}
