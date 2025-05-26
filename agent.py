import config

from pydantic_ai import Agent
from pydantic_ai.models.openai import OpenAIModel
from pydantic_ai.providers.openai import OpenAIProvider

from tools import get_time, get_weather, play_music
from pydantic_ai.common_tools.duckduckgo import duckduckgo_search_tool

def create_agent():
    provider = OpenAIProvider(base_url=config.base_url, api_key=config.api_key)
    model = OpenAIModel(model_name='qwen3:4b', provider=provider)
    prompt = '/no_think Reply in one short sentence'
    agent = Agent(model, system_prompt=prompt, tools=[get_time, get_weather, play_music, duckduckgo_search_tool()])
    return agent

if __name__ == "__main__":
    agent = create_agent()
    #result = agent.run_sync('What is the time right now?')
    #print(result.output)
    #result = agent.run_sync('What is the weather right now?')
    #print(result.output)
    result = agent.run_sync('Play the song Bon Jovi its my life')
    print(result.output)
    #result = agent.run_sync('Who won Monte Carlo Masters tournament in Mens singles in 2024?')
    #print(result.output)
